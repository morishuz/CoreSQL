#include "check.hpp"
#include "../examples/landmark_memory/worker.hpp"
using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        landmarks::Observation a{1, 0, "model-v1", "map-1", 5, 5, landmarks::descriptor(1)};
        landmarks::Observation b{2, 0, "model-v2", "map-1", 5, 5, landmarks::descriptor(1)};
        landmarks::Search search{"model-v1", "map-1", 5, 5, 10, landmarks::descriptor(1), {}};
        std::future<sql::Result> drained;
        {
            landmarks::Worker worker(temp.path / "worker", {2, 4});
            auto batch = std::array{a, b};
            auto accepted = worker.try_ingest(batch);
            CHECK(accepted && accepted->get().changes == 2);
            auto result = worker.try_search(search);
            CHECK(result);
            CHECK((result->get().rows == std::vector<Row>{{std::int64_t{1}, 0.0}}));
            search.frame = "map-2";
            CHECK(worker.try_search(search)->get().rows.empty());
            search.frame = "map-1";
            auto extra = a;
            extra.id = 3;
            accepted = worker.try_ingest(std::array{extra});
            expect(ErrorCode::resource, [&] { (void)accepted->get(); });
            CHECK(worker.try_search(search)->get().rows.size() == 1);
            // Durable repeated updates, reads and failed capacity requests share one owner.
            for (int i = 0; i < 40; ++i) {
                a.seen = i;
                accepted = worker.try_ingest(std::array{a});
                CHECK(accepted && accepted->get().changes == 1);
                CHECK(worker.try_search(search)->get().rows.size() == 1);
            }
            CHECK(worker.try_erase(std::array<std::int64_t, 2>{2, 2})->get().changes == 1);
            CHECK(worker.try_ingest(std::array{extra})->get().changes == 1);
            std::stop_source stop;
            search.options.cancellation = stop.get_token();
            stop.request_stop();
            auto cancelled = worker.try_search(search);
            expect(ErrorCode::cancelled, [&] { (void)cancelled->get(); });
            search.options = {};
            auto invalid = a;
            invalid.model.resize(65, 'x');
            expect(ErrorCode::constraint, [&] { worker.try_ingest(std::array{invalid}); });
            a.seen = 100;
            drained = std::move(*worker.try_ingest(std::array{a}));
        }
        CHECK(drained.get().changes == 1); // Shutdown drains already accepted writes.
        {
            landmarks::Worker reopened(temp.path / "worker", {2, 4});
            CHECK(reopened.try_search(search)->get().rows.size() == 2);
        }
        auto registry = landmarks::registry();
        {
            auto db = Database::open(temp.path / "worker", registry);
            sql::Connection c(db, registry);
            CHECK(c.execute("SELECT seen FROM observations WHERE id=1").rows ==
                  std::vector<Row>{{std::int64_t{100}}});
            db.begin().integrity_check();
        }
        expect(ErrorCode::resource, [&] { landmarks::Worker too_small(temp.path / "worker", {1, 1}); });
        expect(ErrorCode::constraint, [&] { landmarks::Worker invalid(temp.path / "other", {100001, 1}); });
        // A failing request rolls back its whole batch of observations without
        // rolling back neighboring requests sharing the eventual commit.
        const auto grouped_file = temp.path / "grouped";
        {
            auto db = Database::open(grouped_file, registry);
            sql::Connection c(db, registry);
            c.execute(
                "CREATE TABLE observations(id INTEGER PRIMARY KEY,model TEXT NOT NULL,frame TEXT NOT NULL,"
                "x REAL NOT NULL,y REAL NOT NULL,seen INTEGER NOT NULL CHECK(seen<10),descriptor VECTOR(128) "
                "NOT NULL)");
        }
        {
            landmarks::WorkerLimits limits{2, 64};
            limits.batch_wait = std::chrono::milliseconds(10);
            limits.checkpoint_every = 0;
            landmarks::Worker worker(grouped_file, limits);
            auto first = a;
            first.seen = 1;
            auto partial = first;
            partial.seen = 2;
            auto invalid = b;
            invalid.seen = 11;
            auto last = b;
            last.seen = 3;
            auto good1 = worker.try_ingest(std::array{first});
            auto bad = worker.try_ingest(std::array{partial, invalid});
            auto good2 = worker.try_ingest(std::array{last});
            CHECK(good1 && bad && good2);
            CHECK(good1->get().changes == 1);
            expect(ErrorCode::constraint, [&] { (void)bad->get(); });
            CHECK(good2->get().changes == 1);
            auto checkpoint = worker.try_checkpoint();
            CHECK(checkpoint);
            checkpoint->get();
            const auto stats = worker.stats();
            CHECK(stats.accepted == 4 && stats.completed == 4 && stats.failures == 1);
            CHECK(stats.committed_batches >= 1 && stats.committed_batches <= 2 && stats.checkpoints == 1);
            CHECK(stats.samples.size() == 4 && !stats.maintenance_due);
            for (const auto& sample : stats.samples)
                CHECK(sample.total_ms >= sample.queue_ms);
        }
        {
            auto db = Database::open(grouped_file, registry);
            sql::Connection c(db, registry);
            CHECK((c.execute("SELECT id,seen FROM observations ORDER BY id").rows ==
                   std::vector<Row>{{std::int64_t{1}, std::int64_t{1}}, {std::int64_t{2}, std::int64_t{3}}}));
        }
        // Renaming the parent directory preserves the open log but makes the
        // periodic checkpoint's destination unavailable. Report that terminal
        // I/O failure through queued futures and future submissions, not backpressure.
        const auto original = temp.path / "original", moved = temp.path / "moved";
        std::filesystem::create_directory(original);
        {
            landmarks::Worker worker(original / "map", {2, 4});
            std::filesystem::rename(original, moved);
            bool failed = false;
            for (int i = 0; i < 260 && !failed; ++i) {
                try {
                    auto accepted = worker.try_ingest(std::array{a});
                    CHECK(accepted && accepted->get().changes == 1);
                } catch (const Error& error) {
                    CHECK(error.code == ErrorCode::io);
                    failed = true;
                }
            }
            CHECK(failed);
            expect(ErrorCode::io, [&] { worker.try_search(search); });
            expect(ErrorCode::io, [&] { worker.try_erase(std::array<std::int64_t, 1>{1}); });
            expect(ErrorCode::io, [&] { worker.try_ingest(std::array{a}); });
        }
        auto recovered = Database::open(moved / "map", registry);
        sql::Connection after_failure(recovered, registry);
        CHECK(after_failure.execute("SELECT seen FROM observations WHERE id=1").rows ==
              std::vector<Row>{{std::int64_t{100}}});
    });
}
