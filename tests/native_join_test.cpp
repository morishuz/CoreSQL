#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/query.hpp"

using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        std::vector<std::int64_t> trace;
        registry.add(Function{"trace",
                              [](std::span<const Type> args) {
                                  CHECK(args.size() == 1);
                                  return integer();
                              },
                              [&](std::span<const Value> args) -> Value {
                                  trace.push_back(std::get<std::int64_t>(args[0]));
                                  return std::int64_t{1};
                              }});
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute("CREATE TABLE l(k TEXT,id INTEGER)");
        c.execute("CREATE TABLE r(k TEXT,id INTEGER)");
        c.execute("INSERT INTO l VALUES('a',1),('b',2),(NULL,3),('',4)");
        c.execute("INSERT INTO r VALUES('a',10),(NULL,11),('z',12),('a',13),('',14)");
        auto eq = Predicate{column("a", "k"), Compare::equal, column("b", "k")};
        auto callback =
            Predicate{call("trace", {column("b", "id")}), Compare::equal, literal(std::int64_t{1})};
        Query q{"l", {column("a", "id"), column("b", "id")}};
        q.alias = "a";
        q.join = Join{"r", "b", column("a", "k"), column("b", "k")};
        detail::QueryCounters counters;
        {
            detail::QueryCounterScope scope(counters);
            auto result = db.query(q);
            CHECK((result.rows == std::vector<Row>{{std::int64_t{1}, std::int64_t{10}},
                                                   {std::int64_t{1}, std::int64_t{13}},
                                                   {std::int64_t{4}, std::int64_t{14}}}));
        }
        CHECK(counters.candidate_pairs == 3 && counters.hash_build_rows == 5);
        q.join->on = all_of({eq, callback});
        q.join->kind = JoinKind::full;
        auto hashed = db.query(q);
        const auto hashed_trace = trace;
        trace.clear();
        // Double negation is logically identical but forces the nested scan.
        q.join->on = all_of({not_(not_(eq)), callback});
        auto nested = db.query(q);
        CHECK(hashed.rows == nested.rows && hashed_trace == trace);
        CHECK(!trace.empty());
        // Interleaved NULLs and duplicate keys preserve ON callback order for
        // both the generic native buckets and the specialized i64 payload map.
        for (const auto& type : {std::string("INTEGER"), std::string("TEXT")}) {
            c.execute("CREATE TABLE ml(k " + type + ",id INTEGER)");
            c.execute("CREATE TABLE mr(k " + type + ",id INTEGER)");
            c.execute("INSERT INTO ml VALUES(1,1),(9,2),(NULL,3)");
            c.execute("INSERT INTO mr VALUES(NULL,10),(1,11),(2,12),(NULL,13),(1,14),(NULL,15)");
            q.table = "ml";
            q.join->table = "mr";
            q.join->on = all_of({eq, callback});
            trace.clear();
            auto actual = db.query(q);
            auto observed = trace;
            CHECK((observed ==
                   std::vector<std::int64_t>{10, 11, 13, 14, 15, 10, 13, 15, 10, 11, 12, 13, 14, 15}));
            q.join->on = all_of({not_(not_(eq)), callback});
            trace.clear();
            CHECK(db.query(q).rows == actual.rows);
            CHECK(trace == observed);
            c.execute("DROP TABLE ml");
            c.execute("DROP TABLE mr");
        }
        // Arbitrary equality overrides must keep their original nested execution.
        Registry custom(false);
        auto addon = registry.addon(text());
        addon.native_ops = false;
        addon.equal = [](ByteView, const Value&, const Value&) { return true; };
        addon.compare = [](ByteView, const Value&, const Value&) { return 0; };
        addon.hash = [](ByteView, const Value&) { return std::size_t{0}; };
        custom.add(addon);
        Database other(custom);
        auto tx = other.begin();
        tx.create_table("l", {{"k", text()}});
        tx.create_table("r", {{"k", text()}});
        tx.insert("l", {std::string("a")});
        tx.insert("r", {std::string("b")});
        tx.commit();
        Query query{"l", {column("a", "k")}};
        query.alias = "a";
        query.join = Join{"r", "b", column("a", "k"), column("b", "k")};
        detail::QueryCounters fallback;
        {
            detail::QueryCounterScope scope(fallback);
            CHECK(other.query(query).rows.size() == 1);
        }
        CHECK(fallback.hash_build_rows == 0);
        // Native floating equality considers positive and negative zero equal.
        c.execute("CREATE TABLE f(k REAL)");
        c.execute("CREATE TABLE g(k REAL)");
        c.execute("INSERT INTO f VALUES(0.0),(-0.0),(1.5)");
        c.execute("INSERT INTO g VALUES(-0.0),(1.5),(NULL)");
        CHECK(c.execute("SELECT count(*) FROM f JOIN g ON f.k=g.k").rows ==
              std::vector<Row>{{std::int64_t{3}}});
        c.execute("CREATE TABLE d(k DECIMAL(15,2))");
        c.execute("CREATE TABLE e(k DECIMAL(15,2))");
        c.execute("INSERT INTO d VALUES(1.25),(1.25),(2.50)");
        c.execute("INSERT INTO e VALUES(1.25),(NULL)");
        CHECK(c.execute("SELECT count(*) FROM d JOIN e ON d.k=e.k").rows ==
              std::vector<Row>{{std::int64_t{2}}});
    });
}
