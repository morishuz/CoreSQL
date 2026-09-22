#include "check.hpp"
#include "../src/storage.hpp"

#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <sys/wait.h>

using namespace coresql;
namespace {
std::mutex gate_mutex;
std::condition_variable gate;
bool waiting = false, released = false;
thread_local const char* pause_stage = nullptr;
thread_local const char* crash_stage = nullptr;
thread_local const char* fail_stage = nullptr;
void hook(const char* stage) {
    if (crash_stage && std::strcmp(crash_stage, stage) == 0)
        ::_exit(77);
    if (fail_stage && std::strcmp(fail_stage, stage) == 0)
        throw Error(ErrorCode::io, "Injected background checkpoint failure");
    if (pause_stage && std::strcmp(pause_stage, stage) == 0) {
        std::unique_lock lock(gate_mutex);
        waiting = true;
        gate.notify_all();
        gate.wait(lock, [] { return released; });
    }
}
void await_pause() {
    std::unique_lock lock(gate_mutex);
    CHECK(gate.wait_for(lock, std::chrono::seconds(10), [] { return waiting; }));
}
void resume() {
    std::lock_guard lock(gate_mutex);
    released = true;
    gate.notify_all();
}
void reset_gate() {
    std::lock_guard lock(gate_mutex);
    waiting = released = false;
}
void seed(const std::filesystem::path& path) {
    auto db = Database::open(path);
    auto tx = db.begin();
    tx.create_table("t", {{"id", integer()}, {"payload", text()}});
    tx.insert("t", {std::int64_t{1}, std::string(200000, 'a')});
    tx.commit();
}
detail::State changed(const detail::State& base, std::int64_t id) {
    auto next = base;
    auto table = std::make_shared<detail::Table>(*base.tables.at("t"));
    const auto location = table->chunks.begin()->first;
    auto chunk = std::make_shared<detail::Chunk>(*table->chunks.begin()->second.pin());
    chunk->rows[0][0] = id;
    detail::refresh(*chunk);
    table->chunks[location] = chunk;
    detail::refresh(*table);
    next.tables["t"] = std::move(table);
    next.stats = detail::measure(next);
    return next;
}
std::int64_t read(const std::filesystem::path& path) {
    auto db = Database::open(path);
    db.begin().integrity_check();
    return std::get<std::int64_t>(db.query(Query{"t"}).rows[0][0]);
}
} // namespace
int main() {
    return tests([] {
        TempDirectory temp;
        // Both background windows admit acknowledged commits: while the snapshot
        // is being written and after the first tail copy but before publication.
        for (const char* stage : {"background_checkpoint_created", "background_checkpoint_ready"}) {
            const auto path = temp.path / stage;
            seed(path);
            {
                detail::DurableStore store(path);
                auto base = store.recover({});
                Bytes streamed;
                detail::encode_checkpoint(base, [&](ByteView bytes) {
                    streamed.insert(streamed.end(), bytes.begin(), bytes.end());
                });
                CHECK(streamed == detail::encode_changes({}, base));
                auto task = store.prepare_checkpoint();
                expect(ErrorCode::conflict, [&] { store.prepare_checkpoint(); });
                reset_gate();
                auto background = std::async(std::launch::async, [&] {
                    pause_stage = stage;
                    detail::storage_hook(hook);
                    store.write_checkpoint(*task, base);
                    detail::storage_hook(nullptr);
                });
                await_pause();
                auto next = changed(base, 2);
                store.commit(base, next);
                resume();
                background.get();
                CHECK(store.publish_checkpoint(*task));
                task.reset();
                auto final = changed(next, 3);
                store.commit(next, final);
                CHECK(store.checkpoints == 1);
            }
            CHECK(read(path) == 3);
            CHECK(!std::filesystem::exists(path.string() + ".checkpoint.background"));
        }
        // Repeated catch-up keeps the final publication copy at most 256 KiB.
        {
            const auto path = temp.path / "catch-up";
            seed(path);
            {
                detail::DurableStore store(path);
                auto base = store.recover({});
                auto task = store.prepare_checkpoint();
                store.write_checkpoint(*task, base);
                auto next = changed(base, 2);
                store.commit(base, next);
                auto last = changed(next, 3);
                store.commit(next, last);
                CHECK(!store.ready_to_publish(*task));
                store.write_checkpoint(*task, base);
                CHECK(store.ready_to_publish(*task));
                CHECK(store.publish_checkpoint(*task));
            }
            CHECK(read(path) == 3);
        }
        // A schema/synchronous checkpoint replaces the log generation. The
        // earlier candidate is discarded instead of resurrecting its old state.
        {
            const auto path = temp.path / "superseded";
            seed(path);
            {
                detail::DurableStore store(path);
                auto base = store.recover({});
                auto task = store.prepare_checkpoint();
                auto next = changed(base, 4);
                next.schema_epoch = std::make_shared<const int>(1);
                store.commit(base, next);
                store.write_checkpoint(*task, base);
                CHECK(!store.publish_checkpoint(*task));
                task.reset();
                auto cancellation = store.prepare_checkpoint();
                cancellation.reset();
                CHECK(!std::filesystem::exists(path.string() + ".checkpoint.background"));
            }
            CHECK(read(path) == 4);
        }
        // Before rename, a failed temporary file cannot poison the acknowledged
        // log. After rename, uncertain directory synchronization requires reopen.
        for (const char* stage :
             {"background_checkpoint_encoded", "background_checkpoint_synced",
              "background_checkpoint_renamed", "background_checkpoint_directory_synced"}) {
            const auto path = temp.path / (std::string("failure-") + stage);
            seed(path);
            {
                detail::DurableStore store(path);
                auto base = store.recover({});
                auto task = store.prepare_checkpoint();
                auto next = changed(base, 5);
                store.commit(base, next);
                fail_stage = stage;
                detail::storage_hook(hook);
                expect(ErrorCode::io, [&] {
                    store.write_checkpoint(*task, base);
                    store.publish_checkpoint(*task);
                });
                detail::storage_hook(nullptr);
                fail_stage = nullptr;
                const bool published = std::strcmp(stage, "background_checkpoint_renamed") == 0 ||
                                       std::strcmp(stage, "background_checkpoint_directory_synced") == 0;
                CHECK(store.failed == published);
                task.reset();
                if (!published) {
                    auto last = changed(next, 6);
                    store.commit(next, last);
                }
            }
            const bool published = std::strcmp(stage, "background_checkpoint_renamed") == 0 ||
                                   std::strcmp(stage, "background_checkpoint_directory_synced") == 0;
            CHECK(read(path) == (published ? 5 : 6));
        }
        for (const char* stage :
             {"background_checkpoint_created", "half_header", "half_payload", "payload_synced", "half_footer",
              "synced", "background_checkpoint_encoded", "background_checkpoint_ready",
              "background_checkpoint_synced", "background_checkpoint_renamed",
              "background_checkpoint_directory_synced"}) {
            const auto path = temp.path / (std::string("crash-") + stage);
            seed(path);
            const auto child = ::fork();
            CHECK(child >= 0);
            if (child == 0) {
                try {
                    detail::DurableStore store(path);
                    auto base = store.recover({});
                    auto task = store.prepare_checkpoint();
                    auto next = changed(base, 7);
                    store.commit(base, next); // acknowledged before any checkpoint fault
                    crash_stage = stage;
                    detail::storage_hook(hook);
                    store.write_checkpoint(*task, base);
                    store.publish_checkpoint(*task);
                    ::_exit(78);
                } catch (...) {
                    ::_exit(79);
                }
            }
            int status = 0;
            CHECK(::waitpid(child, &status, 0) == child);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
            CHECK(read(path) == 7);
            CHECK(!std::filesystem::exists(path.string() + ".checkpoint.background"));
        }
    });
}
