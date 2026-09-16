#include "check.hpp"
using namespace coresql;
int main() {
    return tests([] {
        std::size_t validations = 0, comparisons = 0;
        const Type type{"test.byte", 1, {}};
        auto value = [&](unsigned n) -> Value { return Opaque(type, {std::byte(n)}); };
        Registry registry;
        registry.add(EncodedTypeAddon{type.id, 1,
                                      [](ByteView parameters) {
                                          if (!parameters.empty())
                                              throw Error(ErrorCode::type, "No parameters");
                                      },
                                      [&](ByteView, ByteView bytes) {
                                          ++validations;
                                          if (bytes.size() != 1)
                                              throw Error(ErrorCode::type, "One byte required");
                                      },
                                      [&](ByteView, ByteView a, ByteView b) {
                                          ++comparisons;
                                          return a[0] == b[0];
                                      },
                                      [&](ByteView, ByteView a, ByteView b) {
                                          ++comparisons;
                                          return (a[0] > b[0]) - (a[0] < b[0]);
                                      }});
        registry.add(Function{"identity", [=](std::span<const Type>) { return type; },
                              [](std::span<const Value> args) -> Value { return args[0]; }});
        registry.add(Function{"invalid", [=](std::span<const Type>) { return type; },
                              [=](std::span<const Value>) -> Value { return Opaque(type, {}); }});
        std::vector<std::int64_t> argument_order;
        registry.add(Function{"mark", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> args) -> Value {
                                  auto n = std::get<std::int64_t>(args[0]);
                                  argument_order.push_back(n);
                                  if (n == -1)
                                      throw Error(ErrorCode::constraint, "argument failure");
                                  return n;
                              }});
        registry.add(Function{"arguments", [](std::span<const Type>) { return integer(); },
                              [](std::span<const Value> args) -> Value {
                                  std::int64_t result = 0;
                                  for (const auto& value : args)
                                      result = 10 * result + std::get<std::int64_t>(value);
                                  return result;
                              }});
        Database db(registry);
        for (std::size_t count : {0, 1, 2, 3, 4, 7}) {
            std::vector<Expr> args;
            std::vector<std::int64_t> expected;
            std::int64_t number = 0;
            for (std::size_t i = 1; i <= count; ++i) {
                expected.push_back(i);
                number = number * 10 + i;
                args.push_back(call("mark", {literal(std::int64_t(i))}));
            }
            argument_order.clear();
            CHECK(db.query(Query{"", {call("arguments", args)}}).rows[0][0] == Value(number));
            CHECK(argument_order == expected);
            if (count >= 2) {
                args[1] = call("mark", {literal(std::int64_t{-1})});
                argument_order.clear();
                expect(ErrorCode::constraint, [&] { db.query(Query{"", {call("arguments", args)}}); });
                CHECK(argument_order == std::vector<std::int64_t>({1, -1}));
            }
        }

        auto seed = db.begin();
        seed.create_table("t", {{"v", type}});
        for (unsigned i = 0; i < 200; ++i)
            seed.insert("t", {value(199 - i)});
        seed.commit();
        for (bool descending : {false, true})
            for (std::size_t limit : {10, 300}) {
                validations = comparisons = 0;
                auto rows = db.query(Query{"t", {}, {}, {Order{column("v"), descending}}, limit}).rows;
                CHECK(validations == 0);
                CHECK(comparisons > 0);
                CHECK(rows.size() == (limit == 10 ? 10 : 200));
                for (std::size_t i = 0; i < rows.size(); ++i)
                    CHECK(std::get<Opaque>(rows[i][0]).bytes()[0] == std::byte(descending ? 199 - i : i));
            }
        Predicate filter{column("v"), Compare::equal, literal(value(17))};
        validations = comparisons = 0;
        CHECK(db.query(Query{"t", {}, filter}).rows.size() == 1);
        CHECK(validations == 1);
        CHECK(comparisons == 200); // Only the literal needs validation.
        validations = 0;
        CHECK(db.query(Query{"t", {}, {}, {Order{call("identity", {column("v")})}}, 10}).rows.size() == 10);
        CHECK(validations == 200); // Every computed key is still checked, exactly once.
        auto invalid = call("invalid", {column("v")});
        expect(ErrorCode::type, [&] { db.query(Query{"t", {}, {}, {Order{invalid}}, 1}); });
        expect(ErrorCode::type,
               [&] { db.query(Query{"t", {}, Predicate{invalid, Compare::equal, column("v")}}); });
        expect(ErrorCode::type, [&] { db.query(Query{"t", {}, {}, {Order{literal(Opaque(type, {}))}}, 0}); });
        auto tx = db.begin();
        expect(ErrorCode::type, [&] { tx.update("t", {{"v", invalid}}, filter); });
        expect(ErrorCode::type, [&] { tx.erase("t", Predicate{invalid, Compare::equal, column("v")}); });
        CHECK(tx.query(Query{"t"}).rows.size() == 200);
        CHECK(tx.erase("t", filter) == 1);
        tx.commit();
        // Bound reductions preserve extension ordering, duplicate elimination and
        // NULL groups. Computed values remain validated before comparison.
        auto reductions = db.begin();
        reductions.create_table("r", {{"v", type, false, {}, {}, true}});
        for (const auto& v : std::vector<Value>{value(2), Null(type), value(1), value(2), Null(type)})
            reductions.insert("r", {v});
        reductions.commit();
        Query distinct{"r", {column("v")}};
        distinct.distinct = true;
        Query grouped{"r", {column("v"), aggregate("count")}};
        grouped.group_by = {column("v")};
        Query counted{"r", {aggregate("count", {column("v")})}};
        counted.select[0].distinct = true;
        std::vector<Query> queries{distinct, grouped, counted};
        for (auto operation : {SetOperation::union_distinct, SetOperation::intersect, SetOperation::except}) {
            Query compound{"r", {column("v")}};
            compound.compounds.push_back({operation, std::make_shared<Query>(Query{"r", {column("v")}})});
            queries.push_back(compound);
        }
        for (auto q : queries) {
            auto expected = db.query(q);
            q.repeatable = true;
            auto actual = db.query(q);
            CHECK(actual.types == expected.types);
            CHECK(actual.rows == expected.rows);
        }
        CHECK(db.query(counted).rows == std::vector<Row>({{std::int64_t{2}}}));
        CHECK(db.query(grouped).rows.size() == 3);
        for (bool repeatable : {false, true}) {
            distinct.repeatable = grouped.repeatable = counted.repeatable = repeatable;
            distinct.select = {call("invalid", {column("v")})};
            grouped.group_by = {call("invalid", {column("v")})};
            grouped.select = {aggregate("count")};
            counted.select[0].arguments = {call("invalid", {column("v")})};
            for (const auto& q : {distinct, grouped, counted})
                expect(ErrorCode::type, [&] { db.query(q); });
        }
        // Public Registry APIs must not trust arbitrary caller-created payloads.
        expect(ErrorCode::type, [&] { registry.compare(Opaque(type, {}), value(0)); });
        expect(ErrorCode::type, [&] { registry.equal(value(0), Opaque(type, {})); });
    });
}
