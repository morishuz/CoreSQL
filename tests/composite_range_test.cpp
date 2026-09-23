#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/query.hpp"
#include <cmath>
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

        c.execute("CREATE TABLE ranges(id INTEGER, x REAL, a REAL NOT NULL)");
        c.execute("BEGIN");
        for (std::int64_t i = 0; i < 1000; ++i)
            c.execute("INSERT INTO ranges VALUES(?,?,?)", Row{i, double(i - 500) / 4, double(i - 500) / 4});
        c.execute("INSERT INTO ranges VALUES(1000,NULL,1000.0)");
        c.execute("COMMIT");
        const std::vector<std::string> predicates{"x BETWEEN -2.0 AND 2.0",
                                                  "x BETWEEN -2.0 AND -1.0",
                                                  "x BETWEEN 2.0 AND -2.0",
                                                  "a > -2.0 AND a < 2.0",
                                                  "a >= -2.0 AND a <= 2.0",
                                                  "a < -124.0",
                                                  "a > 999.0"};
        std::vector<std::vector<Row>> scans;
        for (const auto& p : predicates)
            scans.push_back(c.execute("SELECT id FROM ranges WHERE " + p + " ORDER BY id").rows);
        CHECK(scans[0].size() == 17 && scans[3].size() == 15 && scans[2].empty());
        const std::string null_error = "SELECT id FROM ranges WHERE x BETWEEN -2.0 AND 2.0 AND "
                                       "CASE WHEN id=1000 THEN abs(-9223372036854775808) ELSE 1 END > 0";
        expect(ErrorCode::constraint, [&] { c.execute(null_error); });
        for (const std::string direction : {"ASC", "DESC"}) {
            c.execute("CREATE INDEX rx ON ranges(x " + direction + ")");
            c.execute("CREATE INDEX ra ON ranges(a " + direction + ")");
            for (std::size_t i = 0; i < predicates.size(); ++i) {
                detail::QueryCounters counters;
                detail::QueryCounterScope count(counters);
                CHECK(c.execute("SELECT id FROM ranges WHERE " + predicates[i] + " ORDER BY id").rows ==
                      scans[i]);
                CHECK(counters.rows_tested < 30);
            }
            CHECK(
                c.execute("SELECT id FROM ranges WHERE x BETWEEN ? AND ? ORDER BY id", Row{-2.0, 2.0}).rows ==
                scans[0]);
            expect(ErrorCode::constraint, [&] { c.execute(null_error); });
            c.execute("DROP INDEX rx");
            c.execute("DROP INDEX ra");
        }
        CHECK(std::signbit(std::get<double>(c.execute("SELECT -0.0").rows[0][0])));
        CHECK(c.execute("SELECT CASE WHEN 0 THEN -(-9223372036854775808) ELSE 1 END").rows ==
              std::vector<Row>{{std::int64_t{1}}});
        expect(ErrorCode::constraint, [&] { c.execute("SELECT -(-9223372036854775808)"); });
    });
}
