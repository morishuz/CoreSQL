#include "check.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute(
            "CREATE TABLE t(id INTEGER PRIMARY KEY,m TEXT NOT NULL,f INTEGER NOT NULL,x REAL NOT NULL)");
        const sql::Statement add("INSERT INTO t VALUES(?,?,?,?)");
        for (std::int64_t i = 0; i < 1000; ++i)
            c.execute(add, Row{i, std::string(i % 2 ? "a" : "b"), i % 3, double(i)});
        const sql::Statement query(
            "SELECT id FROM t WHERE m='a' AND f=1 AND x BETWEEN 400.0 AND 450.0 ORDER BY id");
        const auto expected = c.execute(query).rows;
        c.execute("CREATE INDEX local ON t(m DESC,f,x DESC)");
        CHECK(c.execute(query).rows == expected);
        CHECK((c.execute("SELECT id FROM t WHERE m='a' AND f=1 AND x>=990.0 ORDER BY id").rows ==
               std::vector<Row>{{std::int64_t{991}}, {std::int64_t{997}}}));
        CHECK(c.execute("SELECT id FROM t WHERE m='a' AND f=1 AND x BETWEEN 450.0 AND 400.0").rows.empty());
        auto snapshot = db.begin();
        c.execute("UPDATE t SET x=x+1000 WHERE id=403");
        CHECK(c.execute(query).rows.size() + 1 == expected.size());
        CHECK(snapshot
                  .query(Query{"t",
                               {column("x")},
                               Predicate{column("id"), Compare::equal, literal(std::int64_t{403})}})
                  .rows == std::vector<Row>{{403.0}});
        c.execute("UPDATE t SET m=m,f=f,x=x WHERE id=409");
        c.execute("CREATE TABLE nullable(v REAL)");
        c.execute("CREATE INDEX nullable_index ON nullable(v)");
        c.execute("INSERT INTO nullable VALUES(1.0)");
        c.execute("UPDATE nullable SET v=NULL");
        c.execute("UPDATE nullable SET v=2.0");
        db.begin().integrity_check();
        expect(ErrorCode::constraint,
               [&] { c.execute("SELECT id FROM t WHERE m='a' AND abs(-9223372036854775808)>0 AND f=100"); });
    });
}
