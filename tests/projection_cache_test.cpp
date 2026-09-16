#include "check.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        std::vector<std::int64_t> seen;
        registry.add(Function{"observe", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> values) -> Value {
                                  auto n = std::get<std::int64_t>(values[0]);
                                  seen.push_back(n);
                                  if (n == 99)
                                      throw Error(ErrorCode::constraint, "unreachable");
                                  return n;
                              }});
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("t", {{"v", integer()}});
        tx.insert("t", {std::int64_t{1}});
        tx.insert("t", {std::int64_t{2}});
        tx.commit();
        auto value = call("observe", {column("v")});
        Query q{"t", {value, value}};
        auto expected = db.query(q).rows;
        CHECK(seen == std::vector<std::int64_t>({1, 1, 2, 2}));
        seen.clear();
        q.repeatable = true;
        CHECK(db.query(q).rows == expected);
        CHECK(seen == std::vector<std::int64_t>({1, 2}));
        seen.clear();
        q.order_by = {{column("v"), true}};
        CHECK(db.query(q).rows[0][0] == Value(std::int64_t{2}));
        CHECK(seen == std::vector<std::int64_t>({2, 1}));
        seen.clear();
        q.limit = 0;
        CHECK(db.query(q).rows.empty() && seen.empty());
        q.limit = 10;
        q.order_by.clear();
        q.select = {coalesce({literal(std::int64_t{7}), call("observe", {literal(std::int64_t{99})})}),
                    coalesce({literal(std::int64_t{8}), call("observe", {literal(std::int64_t{99})})}), value,
                    value};
        CHECK(db.query(q).rows[0][0] == Value(std::int64_t{7}));
        CHECK(seen == std::vector<std::int64_t>({1, 2}));
        seen.clear();
        q.select = {aggregate("sum", {value}), aggregate("max", {value})};
        CHECK(db.query(q).rows == std::vector<Row>({{std::int64_t{3}, std::int64_t{2}}}));
        CHECK(seen == std::vector<std::int64_t>({1, 2}));
        // Slots must not survive a separate execution, even with the same query.
        seen.clear();
        db.query(q);
        CHECK(seen == std::vector<std::int64_t>({1, 2}));
        seen.clear();
        auto bad = call("observe", {literal(std::int64_t{99})});
        q.select = {choose({{literal(std::int64_t{0}), bad}}, value), value};
        CHECK(db.query(q).rows == expected);
        CHECK(seen == std::vector<std::int64_t>({1, 2}));
        seen.clear();
        q.select = {bad, bad};
        expect(ErrorCode::constraint, [&] { db.query(q); });
        expect(ErrorCode::constraint, [&] { db.query(q); });
        CHECK(seen == std::vector<std::int64_t>({99, 99}));
        seen.clear();
        auto null = call("observe", {literal(Null(integer()))});
        q.select = {null, null};
        auto nulls = db.query(q).rows;
        CHECK(nulls.size() == 2 && is_null(nulls[0][0]) && is_null(nulls[1][1]) && seen.empty());
    });
}
