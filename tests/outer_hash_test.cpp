#include "check.hpp"
#include "../src/state.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        std::vector<Value> calls;
        registry.add(Function{"mark", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> values) -> Value {
                                  calls.push_back(values[0]);
                                  return std::int64_t{1};
                              }});
        registry.add(Function{
            "fail", [](std::span<const Type>) { return integer(); },
            [](std::span<const Value>) -> Value { throw Error(ErrorCode::constraint, "intentional"); }});
        sql::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute("CREATE TABLE a(k INTEGER,v INTEGER)");
        c.execute("CREATE TABLE b(k INTEGER,v INTEGER)");
        c.execute("INSERT INTO a VALUES(1,10),(1,11),(2,20),(NULL,30)");
        c.execute("INSERT INTO b VALUES(1,100),(2,200),(2,201),(NULL,300),(3,400)");
        for (const std::string kind : {"LEFT", "RIGHT", "FULL", "INNER"}) {
            auto run = [&](const std::string& key, detail::QueryCounters& counters) {
                detail::QueryCounterScope scope(counters);
                return c.execute("SELECT a.v,b.v FROM a " + kind + " JOIN b ON " + key + " AND mark(b.v)=1");
            };
            detail::QueryCounters indexed, baseline;
            calls.clear();
            auto actual = run("a.k=b.k", indexed);
            auto trace = calls;
            calls.clear();
            auto expected = run("NOT (NOT (a.k=b.k))", baseline);
            CHECK(actual.rows == expected.rows && trace == calls);
            CHECK(indexed.candidate_pairs < baseline.candidate_pairs);
            CHECK(indexed.hash_build_rows == 5 && baseline.hash_build_rows == 0);
        }
        calls.clear();
        c.execute("SELECT a.v,b.v FROM a LEFT JOIN b ON mark(b.v)=1 AND a.k=b.k");
        CHECK(calls.size() == 20); // A callback before the key cannot be skipped.
        expect(ErrorCode::constraint,
               [&] { c.execute("SELECT * FROM a LEFT JOIN b ON a.k=b.k AND fail()=1"); });
        c.execute("DELETE FROM a");
        c.execute("INSERT INTO a VALUES(99,1)");
        expect(ErrorCode::constraint, [&] {
            c.execute("SELECT * FROM a LEFT JOIN b ON a.k=b.k AND fail()=1");
        }); // NULL right key reaches fail.
        c.execute("DELETE FROM b WHERE k IS NULL");
        CHECK(c.execute("SELECT a.v,b.v FROM a LEFT JOIN b ON b.k=a.k AND fail()=1").rows.size() == 1);
        detail::QueryCounters zero;
        {
            detail::QueryCounterScope scope(zero);
            CHECK(c.execute("SELECT * FROM a FULL JOIN b ON a.k=b.k AND fail()=1 LIMIT 0").rows.empty());
        }
        CHECK(zero.hash_build_rows == 0 && zero.candidate_pairs == 0);
        c.execute("DELETE FROM a");
        CHECK(c.execute("SELECT * FROM a FULL JOIN b ON a.k=b.k AND fail()=1").rows.size() == 4);
        // An explicit ON overrides cross, and conjunctions retain their evaluation order.
        c.execute("CREATE TABLE l(k INTEGER)");
        c.execute("CREATE TABLE r(k INTEGER)");
        c.execute("INSERT INTO l VALUES(1)");
        c.execute("INSERT INTO r VALUES(2)");
        CHECK(c.execute("SELECT l.k,r.k FROM l JOIN r ON l.k=r.k AND l.k>0").rows.empty());
        Query joined{"l", {column("a", "k"), column("b", "k")}};
        joined.alias = "a";
        joined.join = Join{"r", "b"};
        joined.join->cross = true;
        Predicate equality{column("a", "k"), Compare::equal, column("b", "k")};
        Predicate failure{call("fail", {}), Compare::equal, literal(std::int64_t{1})};
        joined.join->on = equality;
        CHECK(db.query(joined).rows.empty());
        joined.join->cross = false;
        joined.join->on = all_of({failure, equality});
        expect(ErrorCode::constraint, [&] { db.query(joined); });
        joined.join->on = all_of({equality, failure});
        CHECK(db.query(joined).rows.empty()); // FALSE short-circuits the callback.
        c.execute("INSERT INTO r VALUES(NULL)");
        expect(ErrorCode::constraint, [&] { db.query(joined); }); // UNKNOWN does not.
        c.execute("DELETE FROM r WHERE k IS NULL");
        c.execute("DELETE FROM l");
        c.execute("INSERT INTO l VALUES(NULL)");
        expect(ErrorCode::constraint, [&] { db.query(joined); });
        joined.limit = 0;
        CHECK(db.query(joined).rows.empty());
    });
}
