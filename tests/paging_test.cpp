#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/pager.hpp"
#include "../src/storage.hpp"
#include "coresql/encoding.hpp"
#include <optional>
#include <thread>
#include <mutex>
using namespace coresql;
namespace {
Predicate by(std::int64_t id) {
    return {column("id"), Compare::equal, literal(id)};
}
Query get(std::int64_t id) {
    return {"items", {}, by(id)};
}
void check_row(const Result& result, std::int64_t id, std::size_t length) {
    CHECK(result.rows.size() == 1);
    CHECK(std::get<std::int64_t>(result.rows[0][0]) == id);
    CHECK(std::get<std::string>(result.rows[0][1]).size() == length);
}
} // namespace
int main() {
    return tests([] {
        TempDirectory temporary;
        const auto path = temporary.path / "paged.core";
        const OpenOptions options{true, 32 * 1024};
        constexpr std::int64_t rows = 5000;
        std::optional<ReadSnapshot> retained;
        {
            auto db = Database::open(path, {}, options);
            auto tx = db.begin();
            tx.create_table("items", {{"id", integer(), true}, {"body", text()}});
            for (std::int64_t i = rows; i-- > 0;)
                tx.insert("items", {i, std::string(2048, char('a' + i % 26))});
            tx.commit();
            CHECK(db.cache_stats().resident_bytes <= options.page_cache_bytes);
            CHECK(db.stats().stored_payload_bytes > options.page_cache_bytes * 100);
            db.trim_cache();
            CHECK(db.cache_stats().resident_bytes == 0);
            for (auto key : {std::int64_t{0}, rows / 2, rows - 1})
                check_row(db.query(get(key)), key, 2048);
            CHECK(db.cache_stats().page_reads >= 3);
            CHECK(db.cache_stats().evictions > 0);
            retained = db.snapshot();
            std::exception_ptr reader_error;
            std::mutex reader_mutex;
            std::vector<std::jthread> readers;
            for (int worker = 0; worker < 3; ++worker)
                readers.emplace_back([snapshot = *retained, worker, &reader_error, &reader_mutex] {
                    try {
                        for (std::int64_t step = 0; step < 150; ++step) {
                            const auto id = (step * 7919 + worker * 31) % rows;
                            check_row(snapshot.query(get(id)), id, 2048);
                        }
                    } catch (...) {
                        std::lock_guard lock(reader_mutex);
                        if (!reader_error)
                            reader_error = std::current_exception();
                    }
                });
            auto cursor = retained->cursor(Query{"items", {column("id"), column("body")}});
            CHECK(cursor.next().has_value());
            db.trim_cache();
            CHECK(db.cache_stats().pinned_bytes > 0);
            // A cursor's current values survive eviction pressure and writes.
            tx = db.begin();
            tx.update("items", {{"body", literal(std::string(3072, 'z'))}}, by(0));
            tx.erase("items", by(1));
            tx.commit();
            check_row(retained->query(get(0)), 0, 2048);
            check_row(db.query(get(0)), 0, 3072);
            CHECK(retained->query(get(1)).rows.size() == 1);
            CHECK(db.query(get(1)).rows.empty());
            std::size_t count = 1;
            while (cursor.next())
                ++count;
            CHECK(count == rows);
            cursor.close();
            readers.clear();
            if (reader_error)
                std::rethrow_exception(reader_error);
            db.checkpoint_async().get();
            db.begin().integrity_check();
        }
        // Snapshot rows and the backing file remain valid after the owner closes.
        check_row(retained->query(get(0)), 0, 2048);
        {
            auto db = Database::open(path, {}, options);
            check_row(db.query(get(0)), 0, 3072);
            CHECK(db.query(get(1)).rows.empty());
            CHECK(std::filesystem::exists(path.string() + ".native-index"));
            const auto reads = db.cache_stats().page_reads;
            db.trim_cache();
            auto cursor = db.cursor(Query{"items", {column("id")}});
            std::size_t count = 0;
            while (cursor.next())
                ++count;
            CHECK(count == rows - 1);
            CHECK(db.cache_stats().page_reads > reads);
            cursor.close();
            db.trim_cache();
            CHECK(db.cache_stats().resident_bytes == 0);
            // More than one index overlay run forces an immutable disk merge.
            auto tx = db.begin();
            for (std::int64_t i = rows; i < rows + 4200; ++i)
                tx.insert("items", {i, std::string("new")});
            expect(ErrorCode::constraint, [&] { tx.insert("items", {rows, std::string("duplicate")}); });
            tx.commit();
            check_row(db.query(get(rows + 4199)), rows + 4199, 3);
        }
        {
            // A stale optional image is rebuilt against the new committed log.
            auto db = Database::open(path, {}, options);
            check_row(db.query(get(rows + 4199)), rows + 4199, 3);
            db.begin().integrity_check();
        }
        {
            // A valid image is reusable without rewriting it on an unchanged open.
            const auto cache = std::filesystem::path(path.string() + ".native-index");
            const auto before = std::filesystem::last_write_time(cache);
            auto db = Database::open(path, {}, options);
            check_row(db.query(get(100)), 100, 2048);
            CHECK(std::filesystem::last_write_time(cache) == before);
        }
        {
            // Forge a self-consistent checksum while swapping two key locations.
            // Row validation must reject this optimization and rebuild it.
            const auto cache = std::filesystem::path(path.string() + ".native-index");
            std::ifstream input(cache, std::ios::binary | std::ios::ate);
            Bytes data(static_cast<std::size_t>(input.tellg()));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(data.data()), data.size());
            input.close();
            encoding::Reader reader(data);
            reader.take(24);
            const auto length = reader.u64();
            reader.take(length);
            reader.take(16);
            const auto offset = data.size() - reader.remaining();
            for (std::size_t i = 0; i < 16; ++i)
                std::swap(data[offset + 8 + i], data[offset + 32 + i]);
            data.resize(data.size() - 8);
            std::uint64_t hash = 14695981039346656037ULL;
            for (auto byte : data) {
                hash ^= std::to_integer<unsigned>(byte);
                hash *= 1099511628211ULL;
            }
            encoding::u64(data, hash);
            std::ofstream output(cache, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(data.data()), data.size());
            output.close();
            auto db = Database::open(path, {}, options);
            check_row(db.query(get(0)), 0, 3072);
        }
        {
            std::ofstream corrupt(path.string() + ".native-index", std::ios::binary | std::ios::trunc);
            corrupt << "corrupt disposable cache";
        }
        {
            auto db = Database::open(path, {}, options);
            check_row(db.query(get(100)), 100, 2048);
        }
        retained.reset();
        {
            // No durable fsync here: isolate long-running scratch extent reuse.
            Database db({}, options);
            auto tx = db.begin();
            tx.create_table("items", {{"id", integer(), true}, {"body", text()}});
            tx.insert("items", {std::int64_t{0}, std::string(8192, 'a')});
            tx.commit();
            const auto original = db.cache_stats().backing_bytes;
            for (int i = 0; i < 200; ++i) {
                tx = db.begin();
                tx.update("items", {{"body", literal(std::string(8192, char('a' + i % 26)))}}, by(0));
                tx.commit();
            }
            CHECK(db.cache_stats().backing_bytes <= original * 3);
        }
        {
            // Materialized scans/sorts keep Match row views alive past eviction.
            Registry registry;
            Database db(registry, OpenOptions{true, 1024});
            auto tx = db.begin();
            tx.create_table("items", {{"id", integer(), true}, {"body", text()}});
            tx.create_table("other", {{"id", integer(), true}, {"body", text()}});
            for (std::int64_t i = 0; i < 80; ++i) {
                tx.insert("items", {i, std::to_string(i) + std::string(4096, 'a')});
                tx.insert("other", {i, std::to_string(i) + std::string(4096, 'b')});
            }
            tx.commit();
            db.trim_cache();
            const auto all = db.query(Query{"items"});
            CHECK(all.rows.size() == 80);
            for (std::int64_t i = 0; i < 80; ++i)
                CHECK(std::get<std::string>(all.rows[i][1]) == std::to_string(i) + std::string(4096, 'a'));
            sql::Connection sql(db, registry);
            const auto ordered = sql.execute("SELECT id,body FROM items ORDER BY body DESC LIMIT 7");
            CHECK(ordered.rows.size() == 7);
            for (std::size_t i = 1; i < ordered.rows.size(); ++i)
                CHECK(std::get<std::string>(ordered.rows[i - 1][1]) >=
                      std::get<std::string>(ordered.rows[i][1]));
            const auto joined = sql.execute(
                "SELECT a.id,a.body,b.body FROM items a JOIN other b ON a.id=b.id ORDER BY a.id DESC");
            CHECK(joined.rows.size() == 80);
            for (std::size_t i = 0; i < joined.rows.size(); ++i) {
                const auto id = std::get<std::int64_t>(joined.rows[i][0]);
                CHECK(id == 79 - static_cast<std::int64_t>(i));
                CHECK(std::get<std::string>(joined.rows[i][1]) ==
                      std::to_string(id) + std::string(4096, 'a'));
                CHECK(std::get<std::string>(joined.rows[i][2]) ==
                      std::to_string(id) + std::string(4096, 'b'));
            }
        }
        {
            // Pinning is explicit; a zero/very small target never invalidates a borrower.
            auto pager = std::make_shared<detail::Pager>(1, Registry{});
            auto columns = std::make_shared<const std::vector<Column>>(std::vector<Column>{{"s", text()}});
            auto chunk = std::make_shared<detail::Chunk>();
            chunk->rows.push_back({std::string(5000, 'x')});
            chunk->rowids.push_back(1);
            detail::refresh(*chunk);
            auto reference = pager->store(std::move(chunk), columns);
            auto pin = reference.pin();
            pager->trim();
            CHECK(pager->stats().overage_bytes > 0);
            CHECK(std::get<std::string>(pin->rows[0][0]).size() == 5000);
            pin.reset();
            pager->trim();
            CHECK(pager->stats().resident_bytes == 0);
            CHECK(std::get<std::string>(reference.pin()->rows[0][0]).size() == 5000);
        }
    });
}
