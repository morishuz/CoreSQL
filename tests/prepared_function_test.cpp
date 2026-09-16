#include "check.hpp"
using namespace coresql;
int main() {
    return tests([] {
        int prepared = 0, invoked = 0, fallback = 0;
        Registry registry;
        registry.add(
            Function{"add_bound",
                     [](std::span<const Type> types) {
                         if (types.size() != 2 || types[0] != integer() || types[1] != integer())
                             throw Error(ErrorCode::type, "Expected integers");
                         return integer();
                     },
                     [&](std::span<const Value> values) -> Value {
                         ++fallback;
                         return std::get<std::int64_t>(values[0]) + std::get<std::int64_t>(values[1]);
                     },
                     false,
                     [&](std::span<const Type>, const Type&) {
                         ++prepared;
                         return [&](std::span<const Value> values) -> Value {
                             ++invoked;
                             return std::get<std::int64_t>(values[0]) + std::get<std::int64_t>(values[1]);
                         };
                     }});
        registry.add(Function{"bad_bound", [](std::span<const Type>) { return integer(); },
                              [](std::span<const Value>) -> Value { return std::int64_t{0}; }, false,
                              [](std::span<const Type>, const Type&) {
                                  return [](std::span<const Value>) -> Value { return std::string("bad"); };
                              }});
        registry.add(Function{"empty_bound", [](std::span<const Type>) { return integer(); },
                              [](std::span<const Value>) -> Value { return std::int64_t{0}; }, false,
                              [](std::span<const Type>, const Type&) {
                                  return std::function<Value(std::span<const Value>)>{};
                              }});
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("t", {{"v", integer(), false, {}, {}, true}});
        for (std::int64_t n : {1, 2, 3})
            tx.insert("t", {n});
        tx.insert("t", {Null(integer())});
        tx.commit();
        Query q{"t", {call("add_bound", {column("v"), literal(std::int64_t{5})})}};
        auto rows = db.query(q).rows;
        CHECK(rows ==
              std::vector<Row>({{std::int64_t{6}}, {std::int64_t{7}}, {std::int64_t{8}}, {Null(integer())}}));
        CHECK(prepared == 1 && invoked == 3 && fallback == 0);
        q.limit = 0;
        CHECK(db.query(q).rows.empty());
        CHECK(prepared == 2 && invoked == 3);
        expect(ErrorCode::type, [&] { db.query(Query{"t", {call("bad_bound", {})}}); });
        expect(ErrorCode::type, [&] { db.query(Query{"t", {call("empty_bound", {})}, {}, {}, 0}); });
        auto update = db.begin();
        expect(ErrorCode::type, [&] { update.update("t", {{"v", call("bad_bound", {})}}); });
        CHECK(update.query(Query{"t"}).rows == db.query(Query{"t"}).rows);
    });
}
