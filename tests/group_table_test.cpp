#include "check.hpp"
#include "../src/group_table.hpp"
#include <limits>
using namespace coresql;
int main() {
    return tests([] {
        auto check = [](const Registry& r) {
            std::vector<Type> types{integer()};
            detail::GroupTable<int> fast(r, types, true), ordered(r, types, false);
            std::vector<Value> values{Null(integer()), std::numeric_limits<std::int64_t>::min(),
                                      std::numeric_limits<std::int64_t>::max()};
            for (std::int64_t i = 999; i >= -999; --i)
                values.push_back(i);
            for (int round = 0; round < 3; ++round)
                for (const auto& value : values) {
                    Row key{value};
                    ++fast.get(key, [] { return 0; });
                    ++ordered.get(key, [] { return 0; });
                }
            std::vector<Row> a, b;
            fast.finish([&](const Row& key, int count) { a.push_back({key[0], std::int64_t(count)}); });
            ordered.finish([&](const Row& key, int count) { b.push_back({key[0], std::int64_t(count)}); });
            CHECK(a == b && a.size() == values.size());
        };
        Registry r;
        check(r);
        Database db(r);
        auto tx = db.begin();
        tx.create_table("g", {{"k", integer(), false, {}, {}, true}, {"v", integer()}});
        for (std::int64_t i = 0; i < 2000; ++i)
            tx.insert("g", {i % 13 ? Value(i % 211) : Value(Null(integer())), i});
        tx.commit();
        Query q{"g", {column("k"), aggregate("sum", {column("v")}), aggregate("count")}};
        q.group_by = {column("k")};
        auto expected = db.query(q).rows;
        q.repeatable = true;
        CHECK(db.query(q).rows == expected);
        q.limit = 1;
        CHECK(db.query(q).rows == std::vector<Row>{expected.front()});
        q.limit = 0;
        CHECK(db.query(q).rows.empty());
        auto custom = r.addon(integer());
        custom.native_ops = false;
        auto compare = custom.compare;
        custom.compare = [compare](ByteView p, const Value& a, const Value& b) { return -compare(p, a, b); };
        Registry other(false);
        other.add(custom);
        check(other);
        detail::GroupTable<int> empty(r, std::vector<Type>{integer()}, true);
        empty.finish([](const Row&, int) { CHECK(false); });
        std::vector<Type> pair{text(), integer()};
        detail::GroupTable<int> hashed(r, pair, true), ordered(r, pair, false);
        for (int round = 0; round < 2; ++round)
            for (auto label : {std::string("a"), std::string("b"), std::string("a")})
                for (const auto& n : {Value(std::int64_t{1}), Value(std::int64_t{2}), Value(Null(integer()))}) {
                    Row key{label, n};
                    ++hashed.get(key, [] { return 0; });
                    ++ordered.get(key, [] { return 0; });
                }
        std::vector<Row> hashed_rows, ordered_rows;
        hashed.finish(
            [&](const Row& key, int count) { hashed_rows.push_back({key[0], key[1], std::int64_t(count)}); });
        ordered.finish([&](const Row& key, int count) {
            ordered_rows.push_back({key[0], key[1], std::int64_t(count)});
        });
        CHECK(hashed_rows == ordered_rows && hashed_rows.size() == 6);
    });
}
