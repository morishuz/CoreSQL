#include "check.hpp"
#include "../src/state.hpp"
#include "coresql/sql.hpp"
#include "coresql/decimal.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        std::vector<Value> calls;
        registry.add(Function{"mark", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> v) -> Value {
                                  calls.push_back(v[0]);
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
        c.execute("INSERT INTO a VALUES(1,10),(1,11),(2,20),(3,30)");
        c.execute("INSERT INTO b VALUES(1,100),(2,200),(2,201),(4,400)");
        auto check = [&](const std::string& first, const std::string& second, bool optimized) {
            const std::string prefix = "SELECT a.v,b.v FROM a,b WHERE ";
            detail::QueryCounters counters;
            sql::Result actual;
            calls.clear();
            {
                detail::QueryCounterScope scope(counters);
                actual = c.execute(prefix + "(" + first + ") OR (" + second + ")");
            }
            auto observed = calls;
            calls.clear();
            auto expected = c.execute(prefix + "NOT (NOT (" + first + ") AND NOT (" + second + "))");
            CHECK(actual.rows == expected.rows);
            CHECK(observed == calls);
            if (optimized) {
                CHECK(actual.rows.size() == 4);
                CHECK(counters.largest_intermediate <= 4);
                CHECK(counters.candidate_pairs == 4);
                CHECK(counters.hash_build_rows == 4 && counters.hash_probes == 4);
            } else {
                CHECK(counters.candidate_pairs == 16);
            }
        };
        check("a.k=b.k AND mark(a.v)=1", "b.k=a.k AND mark(b.v)=1", true);
        check("a.k=b.k AND a.v>=0", "a.k=b.k AND b.v>=0", true); // Overlapping arms, duplicate keys.
        check("mark(a.v)=1 AND a.k=b.k", "a.k=b.k AND b.v>=0", false);
        check("a.k=b.k AND a.v>=0", "a.k<>b.k AND b.v>=0", false);
        check("a.k=b.k AND a.v>=0", "a.v=b.v AND b.v>=0", false);
        // A safe local prefix in every OR arm can narrow the right hash input.
        const std::string guarded = "(a.k=b.k AND b.v=100) OR (a.k=b.k AND b.v=201)";
        detail::QueryCounters guarded_counts;
        sql::Result guarded_rows;
        {
            detail::QueryCounterScope scope(guarded_counts);
            guarded_rows = c.execute("SELECT a.v,b.v FROM a,b WHERE " + guarded);
        }
        CHECK(guarded_rows.rows == c.execute("SELECT a.v,b.v FROM a,b WHERE NOT (NOT (a.k=b.k AND b.v=100) "
                                             "AND NOT (a.k=b.k AND b.v=201))")
                                       .rows);
        CHECK(guarded_rows.rows.size() == 3);
        CHECK(guarded_counts.hash_build_rows == 2 && guarded_counts.candidate_pairs == 3);
        c.execute("CREATE TABLE n(k INTEGER,v INTEGER)");
        c.execute("INSERT INTO n VALUES(1,NULL)");
        // UNKNOWN local guards cannot hide a later failing branch expression.
        expect(ErrorCode::constraint, [&] {
            c.execute(
                "SELECT a.v FROM a,n WHERE (a.k=n.k AND n.v=100 AND fail()=1) OR (a.k=n.k AND n.v=201)");
        });
        detail::QueryCounters zero;
        {
            detail::QueryCounterScope scope(zero);
            CHECK(c.execute("SELECT * FROM a,b WHERE (a.k=b.k AND fail()=1) OR (a.k=b.k AND 1=1) LIMIT 0")
                      .rows.empty());
        }
        CHECK(zero.rows_tested == 0 && zero.candidate_pairs == 0);
        expect(ErrorCode::schema, [&] {
            c.execute("SELECT * FROM a,b WHERE (a.k=b.k AND missing=1) OR (a.k=b.k AND 1=1) LIMIT 0");
        });
        c.execute("UPDATE a SET k=NULL WHERE v=30");
        check("a.k=b.k AND mark(a.v)=1", "a.k=b.k AND mark(b.v)=1", false);
        c.execute("UPDATE a SET k=3 WHERE v=30");
        c.execute("UPDATE b SET k=NULL WHERE v=400");
        check("a.k=b.k AND mark(a.v)=1", "a.k=b.k AND mark(b.v)=1", false);
        c.execute("DELETE FROM a");
        CHECK(c.execute("SELECT * FROM a,b WHERE (a.k=b.k AND fail()=1) OR (a.k=b.k AND fail()=1)")
                  .rows.empty());
        c.execute("INSERT INTO a VALUES(NULL,1)");
        expect(ErrorCode::constraint,
               [&] { c.execute("SELECT * FROM a,b WHERE (a.k=b.k AND fail()=1) OR (a.k=b.k AND 1=1)"); });
        c.execute("CREATE TABLE prices(k INTEGER,p DECIMAL(15,2),d DECIMAL(15,2))");
        c.execute("INSERT INTO prices VALUES(1,100,0.10),(2,50,0.20)");
        auto revenue = c.execute(
            "SELECT sum(p*(1-d)) FROM prices,b WHERE (prices.k=b.k AND p>=50) OR (prices.k=b.k AND p>=100)");
        CHECK(decimals::format(revenue.rows[0][0]) == "170.0000");
        detail::QueryCounters nested, outer;
        {
            detail::QueryCounterScope scope(outer);
            {
                detail::QueryCounterScope inner(nested);
                c.execute("SELECT * FROM b");
            }
            c.execute("SELECT (SELECT count(*) FROM b) FROM prices");
        }
        CHECK(nested.rows_tested == 4);
        CHECK(outer.subquery_executions == 1 && outer.subquery_cache_hits == 1);
        CHECK(detail::active_query_counters == nullptr);
    });
}
