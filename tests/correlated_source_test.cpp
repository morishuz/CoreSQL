#include "check.hpp"
#include "../src/state.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        std::size_t calls = 0;
        registry.add(Function{"mark", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> values) -> Value {
                                  ++calls;
                                  return values[0];
                              }});
        registry.add(Function{
            "fail", [](std::span<const Type>) { return integer(); },
            [](std::span<const Value>) -> Value { throw Error(ErrorCode::constraint, "intentional"); }});
        sql::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute("CREATE TABLE a(k INTEGER)");
        c.execute("CREATE TABLE b(k INTEGER,v INTEGER)");
        c.execute("INSERT INTO a VALUES(1),(1),(2),(NULL)");
        c.execute("INSERT INTO b VALUES(1,10),(1,20),(2,30),(3,40),(NULL,50)");
        detail::QueryCounters counters;
        sql::Result result;
        {
            detail::QueryCounterScope scope(counters);
            result = c.execute("SELECT k,(SELECT sum(v) FROM b WHERE b.k=a.k) FROM a");
        }
        CHECK(result.rows[0][1] == Value(std::int64_t{30}));
        CHECK(result.rows[1][1] == result.rows[0][1]);
        CHECK(result.rows[2][1] == Value(std::int64_t{30}));
        CHECK(is_null(result.rows[3][1]));
        CHECK(counters.subquery_cache_hits >= 1);
        CHECK(counters.correlation_index_rows == 5);
        // Nested references to b must see the full table, not its candidate subset.
        auto nested =
            c.execute("SELECT (SELECT count(*) FROM b WHERE b.k=a.k AND (SELECT count(*) FROM b)=5) FROM a");
        CHECK(nested.rows ==
              std::vector<Row>({{std::int64_t{2}}, {std::int64_t{2}}, {std::int64_t{1}}, {std::int64_t{0}}}));
        c.execute("UPDATE b SET v=11 WHERE v=10");
        CHECK(c.execute("SELECT (SELECT sum(v) FROM b WHERE b.k=a.k) FROM a LIMIT 1").rows[0][0] ==
              Value(std::int64_t{31}));
        // Arbitrary callbacks do not opt into either caching or candidate indexes.
        detail::QueryCounters callbacks;
        {
            detail::QueryCounterScope scope(callbacks);
            c.execute("SELECT (SELECT sum(mark(v)) FROM b WHERE b.k=a.k) FROM a WHERE k=1");
        }
        CHECK(calls == 4 && callbacks.subquery_cache_hits == 0 && callbacks.correlation_index_rows == 0);
        Query sub{"b", {row_id()}, Predicate{column("k"), Compare::equal, parameter("key")}, {}};
        sub.repeatable = true;
        Query outer{"a",
                    {scalar_subquery(sub, {{"key", column("k")}})},
                    Predicate{column("k"), Compare::equal, literal(std::int64_t{2})},
                    {}};
        CHECK(db.query(outer).rows[0][0] == Value(std::int64_t{3}));
        c.execute("DELETE FROM b WHERE v=11");
        CHECK(db.query(outer).rows[0][0] == Value(std::int64_t{3})); // Compacted slots retain row identity.
        // Only the NULL key row can reach fail() for an absent non-NULL key.
        sub.where = all_of({*sub.where, {call("fail", {}), Compare::equal, literal(std::int64_t{1})}});
        outer.select = {exists(sub, {{"key", literal(std::int64_t{99})}})};
        expect(ErrorCode::constraint, [&] { db.query(outer); });
        outer.select = {exists(sub, {{"key", literal(Null(integer()))}})};
        expect(ErrorCode::constraint, [&] { db.query(outer); });
        outer.limit = 0;
        detail::QueryCounters zero;
        {
            detail::QueryCounterScope scope(zero);
            CHECK(db.query(outer).rows.empty());
        }
        CHECK(zero.correlation_index_rows == 0 && zero.subquery_executions == 0);
        // Empty outer input is lazy even for an error-bearing inner query.
        c.execute("DELETE FROM a");
        outer.limit = 1;
        CHECK(db.query(outer).rows.empty());
        // Compare dense negative keys and sparse extreme keys against an
        // equivalent query which deliberately cannot use the candidate index.
        c.execute("CREATE TABLE candidates(k INTEGER)");
        c.execute("CREATE TABLE captures(k INTEGER)");
        c.execute("INSERT INTO captures VALUES(-4),(-1),(0),(3),(99),(NULL),"
                  "(-9223372036854775807-1),(9223372036854775807)");
        auto compare_candidates = [&] {
            auto indexed =
                c.execute("SELECT (SELECT count(*) FROM candidates b WHERE b.k=a.k) FROM captures a");
            auto scanned =
                c.execute("SELECT (SELECT count(*) FROM candidates b WHERE b.k+0=a.k) FROM captures a");
            CHECK(indexed.rows == scanned.rows);
        };
        compare_candidates(); // Empty index.
        c.execute("INSERT INTO candidates VALUES(NULL),(NULL)");
        compare_candidates(); // All NULL keys.
        c.execute("INSERT INTO candidates VALUES(-4),(-3),(-2),(-1),(0),(1),(2),(3),"
                  "(-4),(-3),(-2),(-1),(0),(1),(2),(3)");
        compare_candidates(); // Dense key domain with duplicates and NULLs.
        c.execute("INSERT INTO candidates VALUES(-9223372036854775807-1),(9223372036854775807)");
        compare_candidates(); // Sparse keys spanning the complete signed range.
        c.execute("CREATE TABLE probes(k INTEGER)");
        c.execute("INSERT INTO probes VALUES(-256),(0),(0),(256),(512),(1),(NULL),"
                  "(-9223372036854775807-1),(9223372036854775807)");
        // Presence-filter collisions must preserve duplicate multiplicity.
        auto hashed = c.execute("SELECT a.k,b.k FROM probes a JOIN probes b ON a.k=b.k ORDER BY a.k,b.k");
        auto scanned = c.execute("SELECT a.k,b.k FROM probes a JOIN probes b ON a.k+0=b.k ORDER BY a.k,b.k");
        CHECK(hashed.rows == scanned.rows);
        hashed = c.execute("SELECT a.k,b.k FROM captures a JOIN probes b ON a.k=b.k ORDER BY a.k,b.k");
        scanned = c.execute("SELECT a.k,b.k FROM captures a JOIN probes b ON a.k+0=b.k ORDER BY a.k,b.k");
        CHECK(hashed.rows == scanned.rows); // Includes absent keys as well as filter collisions.
    });
}
