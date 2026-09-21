#include "check.hpp"
#include "coresql/sql.hpp"
#include <cmath>
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.enable_query_cache(true);
        c.execute("CREATE TABLE t(id INTEGER PRIMARY KEY,n INTEGER)");
        c.execute("INSERT INTO t VALUES(1,5),(2,6)");
        const sql::Statement select("SELECT n FROM t WHERE id=?");
        CHECK(c.execute(select, Row{std::int64_t{1}}).rows == std::vector<Row>{{std::int64_t{5}}});
        auto counts = c.query_cache_stats();
        c.execute(select, Row{std::int64_t{1}});
        CHECK(c.query_cache_stats().hits == counts.hits + 1);
        c.execute("UPDATE t SET n=9 WHERE id=1");
        CHECK(c.execute(select, Row{std::int64_t{1}}).rows == std::vector<Row>{{std::int64_t{9}}});
        CHECK(c.execute(select, Row{std::int64_t{2}}).rows == std::vector<Row>{{std::int64_t{6}}});
        CHECK(c.query_cache_stats().misses == counts.misses);
        const sql::Statement range(
            "SELECT n FROM t WHERE id BETWEEN ?1 AND ?2 ORDER BY id LIMIT ?3 OFFSET ?4");
        CHECK(
            (c.execute(range, Row{std::int64_t{1}, std::int64_t{2}, std::int64_t{2}, std::int64_t{0}}).rows ==
             std::vector<Row>{{std::int64_t{9}}, {std::int64_t{6}}}));
        counts = c.query_cache_stats();
        CHECK(
            c.execute(range, Row{std::int64_t{1}, std::int64_t{2}, std::int64_t{1}, std::int64_t{1}}).rows ==
            std::vector<Row>{{std::int64_t{6}}});
        CHECK(c.query_cache_stats().hits == counts.hits + 1);
        expect(ErrorCode::unsupported, [&] {
            c.execute(range, Row{std::int64_t{1}, std::int64_t{2}, std::int64_t{-1}, std::int64_t{0}});
        });
        CHECK(c.execute(select, Row{Null(integer())}).rows.empty());
        CHECK(c.execute(select, Row{std::int64_t{1}}).rows == std::vector<Row>{{std::int64_t{9}}});
        c.execute("BEGIN");
        c.execute("ALTER TABLE t RENAME COLUMN n TO previous");
        expect(ErrorCode::schema, [&] { c.execute(select, Row{std::int64_t{1}}); });
        c.execute("ROLLBACK");
        CHECK(c.execute(select, Row{std::int64_t{1}}).rows == std::vector<Row>{{std::int64_t{9}}});
        c.execute("DROP TABLE t");
        c.execute("CREATE TABLE t(id INTEGER PRIMARY KEY,n TEXT)");
        c.execute("INSERT INTO t VALUES(1,'new')");
        CHECK(c.execute(select, Row{std::int64_t{1}}).rows == std::vector<Row>{{std::string("new")}});
        const sql::Statement parameter("SELECT ?");
        c.execute(parameter, Row{0.0});
        CHECK(std::signbit(std::get<double>(c.execute(parameter, Row{-0.0}).rows[0][0])));
        std::string large(70000, 'x');
        c.execute(parameter, Row{large});
        counts = c.query_cache_stats();
        c.execute(parameter, Row{large});
        CHECK(c.query_cache_stats().hits == counts.hits && c.query_cache_stats().misses == counts.misses + 1);
        c.clear_query_cache();
        CHECK(c.execute(select, Row{std::int64_t{1}}).rows == std::vector<Row>{{std::string("new")}});
    });
}
