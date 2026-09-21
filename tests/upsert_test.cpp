#include "check.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
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
