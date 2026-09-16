#include "check.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry r;
        std::vector<std::int64_t> seen;
        r.add(Function{"observe", [](std::span<const Type>) { return integer(); },
                       [&](std::span<const Value> v) -> Value {
                           seen.push_back(std::get<std::int64_t>(v[0]));
                           return v[0];
                       }});
        r.add(
            Function{"fail", [](std::span<const Type>) { return integer(); },
                     [](std::span<const Value>) -> Value { throw Error(ErrorCode::constraint, "failure"); }});
        Database db(r);
        auto tx = db.begin();
        tx.create_table("a", {{"v", integer(), false, {}, {}, true}});
        tx.create_table("b", {{"v", integer()}});
        tx.create_table("empty", {{"v", integer()}});
        tx.insert("a", {std::int64_t{1}});
        tx.insert("a", {std::int64_t{2}});
        tx.insert("a", {Null(integer())});
        for (std::int64_t v : {1, 2, 3})
            tx.insert("b", {v});
        tx.commit();
        Query q{"a", {column("a", "v"), column("b", "v")}};
        q.alias = "a";
        q.join = Join{"b", "b", literal(std::int64_t{0}), literal(std::int64_t{0}), true};
        auto left = Predicate{call("observe", {column("a", "v")}), Compare::equal, literal(std::int64_t{1})};
        auto right = Predicate{column("b", "v"), Compare::greater, literal(std::int64_t{0})};
        q.where = all_of({left, right});
        auto expected = db.query(q).rows;
        CHECK(seen == std::vector<std::int64_t>({1, 1, 1, 2, 2, 2}));
        seen.clear();
        q.repeatable = true;
        CHECK(db.query(q).rows == expected && seen == std::vector<std::int64_t>({1, 2}));
        seen.clear();
        q.where = all_of({right, left});
        CHECK(db.query(q).rows == expected && seen == std::vector<std::int64_t>({1, 1, 1, 2, 2, 2}));
        // UNKNOWN cannot suppress a later error; empty right input remains lazy.
        q.where = all_of({{literal(Null(integer())), Compare::equal, literal(std::int64_t{1})},
                          {call("fail", {}), Compare::equal, column("b", "v")}});
        expect(ErrorCode::constraint, [&] { db.query(q); });
        q.limit = 0;
        CHECK(db.query(q).rows.empty());
        q.limit = 100;
        q.join->table = "empty";
        CHECK(db.query(q).rows.empty());
        q.join->table = "b";
        // A right-dependent subquery capture is not a left-only predicate.
        Query sub{"a", {}, Predicate{column("v"), Compare::equal, parameter("key")}};
        sub.repeatable = true;
        q.where =
            Predicate{exists(sub, {{"key", column("b", "v")}}), Compare::equal, literal(std::int64_t{1})};
        auto optimized = db.query(q).rows;
        q.repeatable = false;
        CHECK(optimized == db.query(q).rows && optimized.size() == 6);
    });
}
