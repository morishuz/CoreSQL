#include "check.hpp"
#include "coresql/sql.hpp"
#include "coresql/date.hpp"
#include "coresql/timestamp.hpp"
#include "../src/state.hpp"
using namespace coresql;

namespace {
struct RangeProbe final : Index {
    std::shared_ptr<Index> data;
    int mode;
    RangeProbe(const Type& type, const TypeAddon& addon, int mode)
        : data(make_hash_index(type, addon)), mode(mode) {}
    std::shared_ptr<Index> clone() const override {
        auto copy = std::make_shared<RangeProbe>(*this);
        copy->data = data->clone();
        return copy;
    }
    void insert(const Value& value, RowLocation location) override { data->insert(value, location); }
    void erase(const Value& value, RowLocation location) override { data->erase(value, location); }
    std::optional<RowLocation> lookup(const Value& value) const override { return data->lookup(value); }
    std::optional<IndexResult> range(const Value*, const Value*) const override {
        if (mode == 0)
            return {}; // Existing providers need not implement ranges.
        if (mode == 2)
            return IndexResult{{{UINT64_MAX, 0}}, false};
        auto result = data->range(nullptr, nullptr); // Deliberately conservative.
        std::reverse(result->rows.begin(), result->rows.end());
        result->rows.push_back(result->rows.front());
        return result;
    }
};
Predicate interval(std::int64_t lo, std::int64_t hi) {
    return all_of(
        {{column("id"), Compare::greater_equal, literal(lo)}, {column("id"), Compare::less, literal(hi)}});
}
void exercise(Database& db) {
    auto tx = db.begin();
    tx.create_table("items", {{"id", integer(), true}, {"v", integer(), false, {}, {}, true}});
    tx.create_table("scan", {{"id", integer()}, {"v", integer(), false, {}, {}, true}});
    // Key order differs from physical row order; deletions must preserve slots.
    for (std::int64_t i = 0; i < 4096; ++i) {
        const auto key = (i * 7919) % 4096 - 2048;
        const Row row{key, key == 1 ? Value(Null(integer())) : Value(key)};
        tx.insert("items", row);
        tx.insert("scan", row);
    }
    for (const auto key : {INT64_MIN, INT64_MAX}) {
        tx.insert("items", {key, key});
        tx.insert("scan", {key, key});
    }
    tx.commit();
    auto check = [&](Predicate predicate, std::size_t maximum) {
        Query q{"scan", {}, predicate};
        const auto expected = db.query(q).rows;
        q.table = "items";
        detail::QueryCounters counters;
        {
            detail::QueryCounterScope scope(counters);
            CHECK(db.query(q).rows == expected);
        }
        CHECK(counters.rows_tested <= maximum);
        CHECK(db.cursor(q).fetch(5000).rows == expected);
        std::vector<Row> streamed;
        db.query_each(q, [&](std::span<const Value> row) {
            streamed.emplace_back(row.begin(), row.end());
            return true;
        });
        CHECK(streamed == expected);
    };
    check(interval(-7, 8), 16);
    check(interval(-500, 500), 1001);
    check(interval(INT64_MIN, INT64_MAX), 4098);
    check(interval(8, -7), 0);
    check(interval(9000, 9010), 0);
    check({literal(std::int64_t{-7}), Compare::less, column("id")}, 2060);
    check(all_of({{literal(std::int64_t{-7}), Compare::less, column("id")},
                  {literal(std::int64_t{8}), Compare::greater, column("id")}}),
          16);
    check({column("id"), Compare::less, literal(INT64_MIN)}, 1);
    check({column("id"), Compare::greater, literal(INT64_MAX)}, 1);
    check(interval(INT64_MAX, INT64_MAX), 1);
    check(all_of({{column("id"), Compare::greater_equal, literal(std::int64_t{-100})}, interval(-7, 8)}), 16);
    // NULL and a throwing expression before the range must not be bypassed.
    for (const std::string table : {"items", "scan"}) {
        Query q{table,
                {},
                all_of({{column("v"), Compare::equal, literal(std::int64_t{9000})},
                        {call("fail", {}), Compare::equal, literal(std::int64_t{1})},
                        interval(9000, 9010)})};
        expect(ErrorCode::constraint, [&] { db.query(q); });
        q.where =
            all_of({{call("fail", {}), Compare::equal, literal(std::int64_t{1})}, interval(9000, 9010)});
        expect(ErrorCode::constraint, [&] { db.query(q); });
    }
    Query q{"items", {}, interval(-7, 8)};
    auto snapshot = db.begin();
    const auto original = snapshot.query(q).rows;
    tx = db.begin();
    {
        auto savepoint = tx.savepoint();
        tx.erase("items", interval(-7, 8));
        CHECK(tx.query(q).rows.empty());
        savepoint.rollback();
        CHECK(tx.query(q).rows == original);
    }
    tx.erase("items", interval(-7, 0));
    tx.insert("items", {std::int64_t{-3}, std::int64_t{999}});
    tx.update("items", {{"id", literal(std::int64_t{9000})}},
              Predicate{column("id"), Compare::equal, literal(std::int64_t{1})});
    tx.commit();
    CHECK(snapshot.query(q).rows == original);
    CHECK(db.query(q).rows.size() == 8);
    CHECK(db.query(Query{"items", {}, interval(9000, 9001)}).rows.size() == 1);
    auto retained = db.cursor(q);
    const auto current = db.query(q).rows;
    tx = db.begin();
    tx.erase("items", interval(-7, 8));
    tx.commit();
    CHECK(retained.fetch(5000).rows == current);
    CHECK(db.query(q).rows.empty());
    db.begin().integrity_check();
    QueryOptions limited;
    limited.max_work = 5;
    expect(ErrorCode::resource, [&] { db.query(Query{"items", {}, interval(-2000, 2000)}, limited); });

    // Domain types use their registered ordering, independent of SQL spelling.
    const std::vector<std::pair<Type, std::function<Value(std::int64_t)>>> types{
        {real(), [](auto i) -> Value { return double(i) / 4; }},
        {text(), [](auto i) -> Value { return std::string(4, char('a' + i)); }},
        {dates::type(), [](auto i) { return dates::value(i); }},
        {timestamps::type(), [](auto i) { return timestamps::value(i); }}};
    std::size_t sequence = 0;
    for (const auto& [type, value] : types) {
        const auto name = "typed" + std::to_string(sequence++);
        tx = db.begin();
        tx.create_table(name, {{"id", type, true}});
        for (std::int64_t i = 0; i < 20; ++i)
            tx.insert(name, {value(i)});
        tx.commit();
        Query typed{name,
                    {},
                    all_of({{column("id"), Compare::greater, literal(value(7))},
                            {column("id"), Compare::less, literal(value(10))}})};
        detail::QueryCounters counters;
        detail::QueryCounterScope scope(counters);
        CHECK(db.query(typed).rows == (std::vector<Row>{{value(8)}, {value(9)}}));
        CHECK(counters.rows_tested <= 4);
    }
}
} // namespace
int main() {
    return tests([] {
        Registry registry;
        dates::install(registry);
        timestamps::install(registry);
        registry.add(Function{
            "fail", [](std::span<const Type>) { return integer(); },
            [](std::span<const Value>) -> Value { throw Error(ErrorCode::constraint, "intentional"); }});
        {
            Database db(registry);
            exercise(db);
        }
        for (int mode = 0; mode < 3; ++mode) {
            const auto name = "range" + std::to_string(mode);
            registry.add(IndexAddon{name,
                                    [mode](const Type& type, const TypeAddon& addon) {
                                        return std::make_shared<RangeProbe>(type, addon, mode);
                                    },
                                    true});
            Database db(registry);
            auto tx = db.begin();
            tx.create_table("items", {{"id", integer(), true, name}});
            for (std::int64_t i = 0; i < 400; ++i)
                tx.insert("items", {i});
            tx.commit();
            Query q{"items", {}, interval(7, 10)};
            if (mode == 2)
                expect(ErrorCode::state, [&] { db.query(q); });
            else
                CHECK(db.query(q).rows ==
                      (std::vector<Row>{{std::int64_t{7}}, {std::int64_t{8}}, {std::int64_t{9}}}));
        }
        // Sparse keys sharing one hash partition exercise ordered bucket lookup,
        // range boundaries and isolated mutations without dense-key assumptions.
        {
            auto index = make_hash_index(integer(), registry.addon(integer()));
            for (std::int64_t i = 1000; i-- > 0;)
                index->insert(i * 256 + 1, {static_cast<std::uint64_t>(i), 0});
            auto old = index->clone();
            index->erase(std::int64_t{501 * 256 + 1}, {501, 0});
            const Value lo = std::int64_t{500 * 256}, hi = std::int64_t{502 * 256};
            CHECK(index->range(&lo, &hi)->rows == (std::vector<RowLocation>{{500, 0}}));
            CHECK(old->range(&lo, &hi)->rows == (std::vector<RowLocation>{{500, 0}, {501, 0}}));
            expect(ErrorCode::constraint, [&] { index->insert(std::int64_t{1}, {1001, 0}); });
            CHECK(index->lookup(std::int64_t{1}) == (RowLocation{0, 0}));
        }
        TempDirectory temporary;
        const auto path = temporary.path / "range.core";
        const OpenOptions options{false, 16 * 1024};
        {
            auto db = Database::open(path, registry, options);
            exercise(db);
            db.checkpoint();
        }
        for (int reopen = 0; reopen < 2; ++reopen) {
            auto db = Database::open(path, registry, options);
            detail::QueryCounters counters;
            {
                detail::QueryCounterScope scope(counters);
                CHECK(db.query(Query{"items", {}, interval(9000, 9001)}).rows.size() == 1);
                CHECK(db.query(Query{"items", {}, interval(-7, 8)}).rows.empty());
            }
            CHECK(counters.rows_tested <= 3);
            // Force overlay compaction, then range across base and recent changes.
            if (!reopen) {
                auto tx = db.begin();
                for (std::int64_t i = 10000; i < 14200; ++i)
                    tx.insert("items", {i, i});
                tx.commit();
            }
            CHECK(db.query(Query{"items", {}, interval(14090, 14105)}).rows.size() == 15);
            db.begin().integrity_check();
        }
    });
}
