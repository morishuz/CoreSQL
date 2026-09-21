#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/query.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, value TEXT)");
        c.execute("BEGIN");
        for (std::int64_t i = 0; i < 1000; ++i)
            c.execute("INSERT INTO t VALUES(?,?)", Row{i, std::string("value")});
        c.execute("COMMIT");
        Query q{"t", {column("id")}};
        QueryOptions low;
        low.max_work = 10;
        expect(ErrorCode::resource, [&] { db.query(q, low); });
        CHECK(db.query(q).rows.size() == 1000); // Failed scope never leaks into later queries.
        std::stop_source stop;
        QueryOptions cancelled;
        cancelled.cancellation = stop.get_token();
        stop.request_stop();
        expect(ErrorCode::cancelled, [&] { db.query(q, cancelled); });
        QueryOptions expired;
        expired.deadline = std::chrono::steady_clock::now();
        expect(ErrorCode::cancelled, [&] { db.query(q, expired); });
        std::size_t seen = 0;
        detail::QueryCounters counters;
        {
            detail::QueryCounterScope scope(counters);
            auto result = db.query_each(q, [&](auto row) {
                CHECK(std::get<std::int64_t>(row[0]) == static_cast<std::int64_t>(seen));
                return ++seen != 7;
            });
            CHECK(result.rows == 7 && result.stopped && result.types == std::vector<Type>{integer()});
        }
        CHECK(counters.rows_tested == 7); // Does not materialize or visit the other 993 rows.
        std::stop_source during;
        QueryOptions interrupt;
        interrupt.cancellation = during.get_token();
        expect(ErrorCode::cancelled, [&] {
            db.query_each(
                q,
                [&](auto) {
                    during.request_stop();
                    return true;
                },
                interrupt);
        });
        q.order_by = {{column("id")}};
        seen = 0;
        expect(ErrorCode::unsupported, [&] {
            db.query_each(q, [&](auto) {
                ++seen;
                return true;
            });
        });
        CHECK(seen == 0);
        q.order_by.clear();
        q.limit = 0;
        auto empty = db.query_each(q, [&](auto) {
            ++seen;
            return true;
        });
        CHECK(empty.rows == 0 && seen == 0 && !empty.stopped);
        q.limit = 1000;
        auto tx = db.begin();
        seen = 0;
        tx.query_each(q, [&](auto row) {
            if (!seen)
                tx.erase("t");
            CHECK(std::get<std::int64_t>(row[0]) == static_cast<std::int64_t>(seen++));
            return true;
        });
        CHECK(seen == 1000 && tx.query(q).rows.empty());
        tx.rollback();
        sql::Statement select("SELECT id,value FROM t WHERE id>=:first LIMIT 3");
        std::vector<Row> rows;
        auto names = c.query_each(
            select,
            [&](auto row) {
                rows.emplace_back(row.begin(), row.end());
                return true;
            },
            Row{std::int64_t{4}});
        CHECK((names == std::vector<std::string>{"id", "value"}));
        CHECK(rows == c.execute(select, Row{std::int64_t{4}}).rows);
        expect(ErrorCode::resource,
               [&] { c.query(sql::Statement("SELECT count(*) FROM t a CROSS JOIN t b"), {}, low); });
        expect(ErrorCode::unsupported, [&] { c.query(sql::Statement("DELETE FROM t")); });
        c.execute("BEGIN");
        c.execute("UPDATE t SET value='kept' WHERE id=0");
        expect(ErrorCode::resource, [&] { c.query(sql::Statement("SELECT * FROM t"), {}, low); });
        CHECK(c.execute("SELECT value FROM t WHERE id=0").rows == std::vector<Row>{{std::string("kept")}});
        c.execute("ROLLBACK");
        // A nested query invoked by a visitor also consumes its parent's budget.
        seen = 0;
        low.max_work = 100;
        expect(ErrorCode::resource, [&] {
            db.query_each(
                Query{"t", {column("id")}},
                [&](auto) {
                    ++seen;
                    db.query(Query{"t"});
                    return true;
                },
                low);
        });
        CHECK(seen == 1);
        CHECK(db.query(Query{"t"}).rows.size() == 1000);
    });
}
