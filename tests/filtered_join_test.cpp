#include "check.hpp"
#include "../src/state.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        registry.add(Function{"fail", [](std::span<const Type>) { return integer(); },
                              [](std::span<const Value>) -> Value {
                                  throw Error(ErrorCode::constraint, "right filter failed");
                              }});
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("a", {{"k", integer()}});
        tx.create_table("b", {{"k", integer(), false, {}, {}, true}, {"payload", text()}});
        tx.create_table("empty", {{"k", integer()}});
        for (std::int64_t i = 0; i < 4; ++i)
            tx.insert("a", {i});
        for (std::int64_t i = 0; i < 600; ++i)
            tx.insert("b", {i % 7 ? Value(i % 5) : Value(Null(integer())), std::string(2048, 'x')});
        tx.create_index("b", {"by_k", {"k"}});
        tx.commit();
        Query q{"a", {column("a", "k"), column("b", "payload")}};
        q.alias = "a";
        q.join = Join{"b", "b", column("a", "k"), column("b", "k")};
        q.join->right_where = Predicate{column("k"), Compare::greater, literal(std::int64_t{1})};
        auto same = [&](Query query) {
            query.repeatable = false;
            auto expected = db.query(query);
            query.repeatable = true;
            detail::QueryCounters counters;
            detail::QueryCounterScope scope(counters);
            auto actual = db.query(query);
            CHECK(actual.types == expected.types && actual.rows == expected.rows);
            return counters;
        };
        for (auto kind : {JoinKind::inner, JoinKind::left, JoinKind::right, JoinKind::full}) {
            q.join->kind = kind;
            same(q);
        }
        q.join->kind = JoinKind::inner;
        q.select = {aggregate("count")};
        auto counters = same(q);
        CHECK(counters.largest_intermediate < 10); // No full-width filtered right copy.
        q.join->right_where = Predicate{column("k"), Compare::equal, literal(std::int64_t{99})};
        CHECK(same(q).largest_intermediate < 10);
        // Filtered final joins can stream into grouping; duplicates keep multiplicity.
        Query chain{"a", {column("a", "k"), aggregate("count")}};
        chain.alias = "a";
        chain.group_by = {column("a", "k")};
        chain.joins = {Join{"a", "x", column("a", "k"), column("x", "k")},
                       Join{"b", "b", column("x", "k"), column("b", "k")}};
        chain.joins.back().right_where = Predicate{column("k"), Compare::greater, literal(std::int64_t{1})};
        CHECK(same(chain).largest_intermediate < 10);
        // The original right table remains visible to nested subqueries.
        q.select = {scalar_subquery(Query{"b", {aggregate("count")}})};
        q.join->right_where = Predicate{column("k"), Compare::equal, literal(std::int64_t{2})};
        same(q);
        // Right filtering is eager, including empty left inputs and false WHERE.
        q.select = {aggregate("count")};
        q.where = Predicate{literal(std::int64_t{0}), Compare::equal, literal(std::int64_t{1})};
        q.join->right_where = Predicate{call("fail", {}), Compare::equal, literal(std::int64_t{1})};
        for (bool repeatable : {false, true}) {
            q.repeatable = repeatable;
            for (const auto& left : {"a", "empty"}) {
                q.table = left;
                expect(ErrorCode::constraint, [&] { db.query(q); });
            }
            q.limit = 0;
            CHECK(db.query(q).rows.empty());
            q.join->right_where = Predicate{column("missing"), Compare::equal, literal(std::int64_t{1})};
            expect(ErrorCode::schema, [&] { db.query(q); });
            q.join->right_where = Predicate{call("fail", {}), Compare::equal, literal(std::int64_t{1})};
            q.limit = std::numeric_limits<std::size_t>::max();
        }
    });
}
