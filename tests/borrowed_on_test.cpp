#include "check.hpp"
#include "../src/state.hpp"
using namespace coresql;

int main() {
    return tests([] {
        Registry registry;
        std::vector<std::string> trace;
        for (const std::string phase : {"on", "where", "project"})
            registry.add(Function{phase, [](std::span<const Type>) { return integer(); },
                                  [&, phase](std::span<const Value> values) -> Value {
                                      trace.push_back(phase);
                                      return values[0];
                                  }});
        registry.add(Function{"on.fail", [](std::span<const Type>) { return integer(); },
                              [](std::span<const Value> values) -> Value {
                                  if (values[0] == Value(std::int64_t{2}))
                                      throw Error(ErrorCode::constraint, "ON failure");
                                  return std::int64_t{1};
                              }});
        registry.add(
            Function{"where.fail", [](std::span<const Type>) { return integer(); },
                     [](std::span<const Value>) -> Value { throw Error(ErrorCode::type, "WHERE failure"); }});
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("l", {{"k", integer()}, {"s", text()}});
        tx.create_table("r", {{"k", integer()}, {"v", integer()}});
        const std::string payload(1024, 'x');
        tx.insert("l", {std::int64_t{1}, payload});
        tx.insert("l", {std::int64_t{1}, payload + "y"});
        tx.insert("r", {std::int64_t{1}, std::int64_t{1}});
        tx.insert("r", {std::int64_t{1}, std::int64_t{2}});
        tx.commit();
        Query q{"l", {column("a", "s"), call("project", {column("b", "v")})}};
        q.alias = "a";
        q.join = Join{"r", "b"};
        q.join->cross = true; // Explicit ON always takes precedence.
        Predicate equality{column("a", "k"), Compare::equal, column("b", "k")};
        q.join->on = all_of({equality, Predicate{call("on", {column("b", "v")}), Compare::greater,
                                                 literal(std::int64_t{0})}});
        q.where = Predicate{call("where", {column("b", "v")}), Compare::greater, literal(std::int64_t{0})};
        for (bool repeatable : {false, true})
            for (bool sorted : {false, true})
                for (std::size_t limit : {std::size_t{0}, std::size_t{1}, std::size_t{10}}) {
                    q.repeatable = repeatable;
                    q.limit = limit;
                    q.order_by = sorted
                                     ? std::vector<Order>{{column("a", "s"), true}, {column("b", "v"), true}}
                                     : std::vector<Order>{};
                    auto reference = q;
                    reference.join->kind = JoinKind::left; // Every left row matches; materialized reference.
                    trace.clear();
                    auto expected = db.query(reference);
                    auto expected_trace = trace;
                    trace.clear();
                    detail::QueryCounters counters;
                    detail::QueryCounterScope counter_scope(counters);
                    auto actual = db.query(q);
                    CHECK(actual.rows == expected.rows && actual.types == expected.types);
                    CHECK(trace == expected_trace);
                    CHECK(counters.intermediate_rows == 0);
                }
        q.limit = 1;
        q.order_by.clear();
        q.join->on = all_of({equality, Predicate{call("on.fail", {column("b", "v")}), Compare::equal,
                                                 literal(std::int64_t{1})}});
        q.where = Predicate{call("where.fail", {}), Compare::equal, literal(std::int64_t{1})};
        // Later ON errors precede earlier WHERE errors, even with LIMIT 1.
        expect(ErrorCode::constraint, [&] { db.query(q); });
        q.join->on =
            all_of({equality, Predicate{literal(Null(integer())), Compare::equal, literal(std::int64_t{0})}});
        CHECK(db.query(q).rows.empty()); // UNKNOWN ON excludes the pair before WHERE can throw.
        q.join->on = equality;
        q.where.reset();
        q.limit = 10;
        q.select.clear(); // SELECT * retains both sides in schema order.
        auto owned = db.query(q);
        CHECK(owned.rows.size() == 4 && owned.rows[0].size() == 4);
        CHECK(owned.rows[0][1] == Value(payload));
        Query subquery{"r", {column("v")}, Predicate{column("v"), Compare::equal, parameter("v")}};
        q.select = {scalar_subquery(subquery, {{"v", column("b", "v")}})};
        CHECK(db.query(q).rows ==
              std::vector<Row>({{std::int64_t{1}}, {std::int64_t{2}}, {std::int64_t{1}}, {std::int64_t{2}}}));
        q.select = {aggregate("count")}; // Grouping still uses the general fallback.
        CHECK(db.query(q).rows[0][0] == Value(std::int64_t{4}));
        auto erase = db.begin();
        erase.erase("l");
        erase.erase("r");
        erase.commit();
        CHECK(owned.rows[0][1] == Value(payload));
    });
}
