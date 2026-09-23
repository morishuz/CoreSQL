#include "check.hpp"
#include "coresql/sql.hpp"
#include <bit>
using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
        for (std::size_t cache : {std::size_t{0}, std::size_t{32768}}) {
            auto db = Database::open(temp.path / ("noop" + std::to_string(cache)), registry,
                                     OpenOptions{false, cache});
            sql::Connection c(db, registry);
            c.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, v REAL, body TEXT)");
            c.execute("INSERT INTO t VALUES(1,0.0,'payload')");
            const auto bytes = db.storage_stats().bytes_written;
            const auto pages = db.cache_stats().page_writes;
            auto result = c.execute("INSERT INTO t VALUES(1,0.0,'ignored') ON CONFLICT(id) "
                                    "DO UPDATE SET v=excluded.v RETURNING *");
            CHECK(result.changes == 1 && result.rows == c.execute("SELECT * FROM t").rows);
            CHECK(db.storage_stats().bytes_written == bytes);
            CHECK(db.cache_stats().page_writes == pages);
            {
                auto tx = db.begin();
                tx.update("t", {{"v", literal(-0.0)}},
                          Predicate{column("id"), Compare::equal, literal(std::int64_t{1})});
                tx.commit();
                CHECK(db.storage_stats().bytes_written > bytes);
                CHECK(std::bit_cast<std::uint64_t>(std::get<double>(
                          c.execute("SELECT v FROM t").rows[0][0])) == std::bit_cast<std::uint64_t>(-0.0));
            }
            auto stale = db.begin();
            c.execute("UPDATE t SET body='changed' WHERE id=1");
            stale.update("t", {{"body", column("body")}},
                         Predicate{column("id"), Compare::equal, literal(std::int64_t{1})});
            expect(ErrorCode::conflict, [&] { stale.commit(); });
        }
        {
            auto db = Database::open(temp.path / "db", registry);
            sql::Connection c(db, registry);
            c.execute("CREATE TABLE objects(id INTEGER PRIMARY KEY,label TEXT UNIQUE,value INTEGER "
                      "CHECK(value>=0))");
            c.execute("INSERT INTO objects VALUES(1,'a',2),(2,'b',3)");
            auto result = c.execute("INSERT INTO objects VALUES(1,'unused',5) ON CONFLICT(id) DO UPDATE SET "
                                    "value=objects.value+excluded.value RETURNING *");
            CHECK((result.changes == 1 &&
                   result.rows == std::vector<Row>{{std::int64_t{1}, std::string("a"), std::int64_t{7}}}));
            result = c.execute("INSERT INTO objects VALUES(9,'a',5) ON CONFLICT(label) DO UPDATE SET "
                               "value=excluded.value WHERE value>100 RETURNING id");
            CHECK(result.changes == 0 && result.rows.empty());
            result = c.execute("INSERT INTO objects VALUES(9,'a',5) ON CONFLICT DO NOTHING RETURNING id");
            CHECK(result.changes == 0 && result.rows.empty());
            expect(ErrorCode::constraint,
                   [&] { c.execute("INSERT INTO objects VALUES(9,'a',-1) ON CONFLICT DO NOTHING"); });
            expect(ErrorCode::constraint,
                   [&] { c.execute("INSERT INTO objects VALUES(9,'a',5) ON CONFLICT(id) DO NOTHING"); });
            expect(ErrorCode::schema,
                   [&] { c.execute("INSERT INTO objects VALUES(9,'c',5) ON CONFLICT(value) DO NOTHING"); });
            expect(ErrorCode::schema, [&] {
                c.execute("INSERT INTO objects VALUES(9,'c',5) ON CONFLICT(id) DO UPDATE SET absent=1");
            });
            expect(ErrorCode::schema, [&] {
                c.execute("INSERT INTO objects VALUES(9,'c',5) ON CONFLICT(id) DO UPDATE SET "
                          "value=excluded.absent");
            });
            expect(ErrorCode::constraint, [&] {
                c.execute(
                    "INSERT INTO objects VALUES(3,'c',5),(1,'a',1) ON CONFLICT(id) DO UPDATE SET value=-1");
            });
            CHECK(c.execute("SELECT count(*) FROM objects").rows == std::vector<Row>{{std::int64_t{2}}});
            c.execute("CREATE TABLE refs(id INTEGER REFERENCES objects(id))");
            c.execute("INSERT INTO refs VALUES(1)");
            c.execute(
                "INSERT INTO objects VALUES(1,'a',10) ON CONFLICT(id) DO UPDATE SET value=excluded.value");
            expect(ErrorCode::constraint, [&] {
                c.execute("INSERT INTO objects VALUES(1,'a',10) ON CONFLICT(id) DO UPDATE SET id=3");
            });
            c.execute("CREATE TABLE pairs(a INTEGER,b INTEGER,n INTEGER,PRIMARY KEY(a,b))");
            c.execute("INSERT INTO pairs VALUES(1,2,3)");
            c.execute("INSERT INTO pairs VALUES(1,2,4) ON CONFLICT(b,a) DO UPDATE SET n=n+excluded.n");
            CHECK(c.execute("SELECT n FROM pairs").rows == std::vector<Row>{{std::int64_t{7}}});
            c.execute("INSERT INTO objects(label,value) VALUES('a',20),('c',30) ON CONFLICT(label) DO UPDATE "
                      "SET value=excluded.value");
            CHECK((c.execute("SELECT id,value FROM objects ORDER BY id").rows ==
                   std::vector<Row>{{std::int64_t{1}, std::int64_t{20}},
                                    {std::int64_t{2}, std::int64_t{3}},
                                    {std::int64_t{3}, std::int64_t{30}}}));
            db.checkpoint();
        }
        auto db = Database::open(temp.path / "db", registry);
        sql::Connection c(db, registry);
        CHECK(c.execute("SELECT value FROM objects WHERE id=1").rows == std::vector<Row>{{std::int64_t{20}}});
        db.begin().integrity_check();
    });
}
