#include "check.hpp"
#include "coresql/sql.hpp"
#include <atomic>
#include <barrier>
#include <thread>
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        Database ordinary(registry);
        expect(ErrorCode::state, [&] { (void)ordinary.snapshot(); });
        TempDirectory temp;
        std::optional<ReadSnapshot> retained;
        {
            auto db = Database::open(temp.path / "snapshots", registry, {.concurrent_reads = true});
            sql::Connection writer(db, registry);
            writer.execute("CREATE TABLE t(id INTEGER PRIMARY KEY,n INTEGER NOT NULL)");
            writer.execute("INSERT INTO t VALUES(1,0),(2,0)");
            retained = db.snapshot();
            std::barrier start(4);
            std::atomic<bool> done = false;
            std::atomic<int> reads = 0;
            std::vector<std::exception_ptr> errors(3);
            std::vector<std::jthread> readers;
            for (std::size_t i = 0; i < errors.size(); ++i)
                readers.emplace_back([&, i] {
                    try {
                        start.arrive_and_wait();
                        const sql::Statement select("SELECT n FROM t ORDER BY id");
                        do {
                            sql::ReadConnection read(db.snapshot());
                            const auto rows = read.query(select).rows;
                            CHECK(rows.size() == 2 && rows[0] == rows[1]);
                            const auto stable = read.query(select).rows;
                            CHECK(stable == rows);
                            ++reads;
                        } while (!done);
                    } catch (...) {
                        errors[i] = std::current_exception();
                    }
                });
            start.arrive_and_wait();
            std::optional<std::future<void>> checkpoint;
            for (int i = 1; i <= 80; ++i) {
                writer.execute("UPDATE t SET n=?", Row{std::int64_t{i}});
                if (i == 20)
                    checkpoint.emplace(db.checkpoint_async());
            }
            done = true;
            readers.clear();
            checkpoint->get();
            for (auto error : errors)
                if (error)
                    std::rethrow_exception(error);
            CHECK(reads > 0);
            sql::ReadConnection old(*retained);
            sql::Statement source("SELECT n FROM t"), moved(std::move(source));
            expect(ErrorCode::state, [&] { old.query(source); });
            sql::TypeAdapters no_adapters(false);
            sql::Statement foreign("SELECT 1", no_adapters);
            expect(ErrorCode::type, [&] { old.query(foreign); });
            CHECK((old.query(sql::Statement("SELECT n FROM t ORDER BY id")).rows ==
                   std::vector<Row>{{std::int64_t{0}}, {std::int64_t{0}}}));
            expect(ErrorCode::unsupported, [&] { old.query(sql::Statement("DELETE FROM t")); });
            // A cursor retains the snapshot and can outlive the reader connection.
            auto cursor = old.cursor(sql::Statement("SELECT n FROM t"));
            CHECK(cursor.next() == Row{std::int64_t{0}});
            auto first = db.begin(), stale = db.begin();
            first.update("t", {{"n", literal(std::int64_t{81})}});
            first.commit();
            expect(ErrorCode::conflict, [&] { stale.commit(); });
        }
        CHECK(retained->query(Query{"t", {column("n")}}).rows.size() == 2);
        auto reopened = Database::open(temp.path / "snapshots", registry);
        sql::Connection check(reopened, registry);
        CHECK((check.execute("SELECT n FROM t ORDER BY id").rows ==
               std::vector<Row>{{std::int64_t{81}}, {std::int64_t{81}}}));
        reopened.begin().integrity_check();
    });
}
