#include "check.hpp"
#include "coresql/sql.hpp"
#include "coresql/timestamp.hpp"
#include "../src/query.hpp"
using namespace coresql;

int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        timestamps::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute("CREATE TABLE events(id INTEGER PRIMARY KEY,device INTEGER NOT NULL,ts INTEGER NOT "
                  "NULL,body TEXT NOT NULL)");
        c.execute("BEGIN");
        for (std::int64_t i = 0; i < 3000; ++i)
            c.execute("INSERT INTO events VALUES(?,?,?,?)",
                      Row{(i * 7919) % 3000, i % 3, i, std::string(100, 'a')});
        c.execute("COMMIT");
        std::vector<std::string> queries{
            "SELECT id,ts FROM events WHERE device=1 ORDER BY ts DESC LIMIT 20",
            "SELECT id,ts FROM events WHERE device=1 AND ts>1501 ORDER BY ts LIMIT 20",
            "SELECT body FROM events WHERE 1=device AND 1501<ts AND ts<1700 ORDER BY ts DESC LIMIT 20 OFFSET "
            "2",
            "SELECT id FROM events WHERE device=9 ORDER BY ts LIMIT 20",
            "SELECT id FROM events WHERE device=1 AND ts>2000 AND ts<1000 ORDER BY ts LIMIT 20",
            "SELECT id FROM events WHERE device=1 AND ts>=1501 AND ts<=1501 ORDER BY ts LIMIT 20",
            "SELECT id FROM events WHERE device=1 ORDER BY device DESC,ts DESC LIMIT 20",
            "SELECT id FROM events WHERE device=1 AND id<10 ORDER BY ts LIMIT 20",
            "SELECT id FROM events WHERE device=1 AND ts>1501 ORDER BY ts LIMIT 0"};
        std::vector<std::vector<Row>> expected;
        for (const auto& query : queries)
            expected.push_back(c.execute(query).rows);
        for (const auto& direction : {"ASC", "DESC"}) {
            c.execute(std::string("CREATE INDEX ordered ON events(device DESC,ts ") + direction + ",id)");
            for (std::size_t i = 0; i < queries.size(); ++i) {
                detail::QueryCounters counters;
                {
                    detail::QueryCounterScope scope(counters);
                    CHECK(c.execute(queries[i]).rows == expected[i]);
                }
                if (i < 2) {
                    CHECK(counters.rows_tested <= 21);
                    CHECK(counters.ordered_entries <= 22);
                }
                auto cursor = c.cursor(sql::Statement(queries[i]));
                std::vector<Row> streamed;
                while (auto row = cursor.next())
                    streamed.push_back(std::move(*row));
                CHECK(streamed == expected[i]);
            }
            c.execute("DROP INDEX ordered");
        }
        // Whole ORDER BY ties retain physical scan order, including the LIMIT
        // boundary, reverse traversal and extra index suffixes.
        c.execute("CREATE TABLE ties(id INTEGER PRIMARY KEY,d INTEGER NOT NULL,t INTEGER,x REAL,n INTEGER)");
        c.execute("BEGIN");
        for (std::int64_t i = 0; i < 600; ++i)
            c.execute("INSERT INTO ties VALUES(?,?,?,?,?)",
                      Row{600 - i, i % 2, i % 7 ? Value(i % 5) : Value(Null(integer())), double(i % 3),
                          i % 11 ? Value(i) : Value(Null(integer()))});
        c.execute("COMMIT");
        std::vector<std::string> tie_queries;
        for (const std::string order : {"t", "t DESC", "t,x DESC", "t DESC,x", "d DESC,t DESC"})
            for (const std::string filter : {"d=1", "d=1 AND t>=1", "d=1 AND n>50", "d=1 AND t<3 AND t>1"})
                tie_queries.push_back("SELECT id,t,x FROM ties WHERE " + filter + " ORDER BY " + order +
                                      " LIMIT 17 OFFSET 3");
        expected.clear();
        for (const auto& q : tie_queries)
            expected.push_back(c.execute(q).rows);
        c.execute("CREATE INDEX tied ON ties(d,t,x DESC,id)");
        for (std::size_t i = 0; i < tie_queries.size(); ++i) {
            CHECK(c.execute(tie_queries[i]).rows == expected[i]);
            CHECK(c.cursor(sql::Statement(tie_queries[i])).fetch(1000).rows == expected[i]);
        }
        c.execute("DELETE FROM ties WHERE id=590 OR id=580 OR id=570");
        auto snapshot = db.begin();
        Query q{"ties",
                {column("id"), column("t")},
                Predicate{column("d"), Compare::equal, literal(std::int64_t{1})}};
        q.order_by = {{column("t"), true}};
        q.limit = 17;
        const auto before = snapshot.query(q).rows;
        auto retained = db.cursor(q);
        CHECK(retained.fetch(2).rows == std::vector<Row>(before.begin(), before.begin() + 2));
        c.execute("UPDATE ties SET t=900 WHERE d=1");
        CHECK(retained.fetch(1000).rows == std::vector<Row>(before.begin() + 2, before.end()));
        CHECK(snapshot.query(q).rows == before);
        auto tx = db.begin();
        const auto stable = tx.query(q).rows;
        {
            auto savepoint = tx.savepoint();
            tx.erase("ties");
            CHECK(tx.cursor(q).fetch(100).rows.empty());
            savepoint.rollback();
        }
        CHECK(tx.query(q).rows == stable);
        tx.rollback();
        db.begin().integrity_check();

        // A late error must still be reported, even after enough matches exist.
        c.execute("CREATE INDEX event_order ON events(device,ts)");
        expect(ErrorCode::constraint, [&] {
            c.execute("SELECT id FROM events WHERE device=1 AND CASE WHEN ts=2998 THEN "
                      "abs(-9223372036854775808) ELSE 1 END>0 ORDER BY ts LIMIT 1");
        });
        expect(ErrorCode::constraint, [&] {
            c.execute("SELECT id FROM events WHERE device=1 ORDER BY CASE WHEN ts=2998 THEN "
                      "abs(-9223372036854775808) ELSE ts END LIMIT 1");
        });
        expect(ErrorCode::schema, [&] { c.execute("SELECT missing FROM events ORDER BY ts LIMIT 0"); });
        auto native = db.begin();
        native.create_table("native", {{"ts", timestamps::type()}, {"id", integer()}});
        for (std::int64_t i = 0; i < 100; ++i)
            native.insert("native", {timestamps::value(i), i});
        native.create_index("native", {"timestamp_order", {"ts"}});
        native.commit();
        Query timestamp{
            "native", {column("id")}, Predicate{column("ts"), Compare::less, literal(timestamps::value(50))}};
        timestamp.order_by = {{column("ts"), true}};
        timestamp.limit = 3;
        {
            detail::QueryCounters counters;
            detail::QueryCounterScope scope(counters);
            CHECK((db.query(timestamp).rows ==
                   std::vector<Row>{{std::int64_t{49}}, {std::int64_t{48}}, {std::int64_t{47}}}));
            CHECK(counters.rows_tested <= 4 && counters.ordered_entries <= 5);
        }
        CHECK(db.cursor(timestamp).fetch(10).rows == db.query(timestamp).rows);
        QueryOptions small;
        small.max_buffer_bytes = 128;
        expect(ErrorCode::resource, [&] { db.query(q, small); });
        auto limited = db.cursor(q, small);
        expect(ErrorCode::resource, [&] { limited.next(); });
        CHECK(limited.stats().closed);
        auto suspended = db.cursor(q);
        suspended.next();
        suspended.close(); // Destroy suspended tie buffers without a live QueryControl.
        small.max_buffer_bytes = 1024 * 1024;
        small.max_work = 10;
        expect(ErrorCode::resource, [&] { db.query(q, small); });
    });
}
