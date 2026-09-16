#include "check.hpp"
#include "coresql/spatial.hpp"
#include "coresql/timestamp.hpp"
#include "../src/state.hpp"
#include <algorithm>
#include <random>

using namespace coresql;
namespace {
Query nearby(spatial::Box box) {
    Query q{"objects", {column("id")}, {}, {Order{column("id")}}};
    q.search = IndexSearch{"box", "overlaps", spatial::value(box)};
    return q;
}
Predicate key(std::int64_t n) { return {column("id"), Compare::equal, literal(n)}; }
// Independent test index deliberately fails after mutating its private state.
// erase need not have a strong guarantee: the engine must discard the statement.
struct FailingIndex final : Index {
    std::shared_ptr<Index> inner;
    std::shared_ptr<bool> fail;
    FailingIndex(std::shared_ptr<Index> i, std::shared_ptr<bool> f) : inner(std::move(i)), fail(std::move(f)) {}
    std::shared_ptr<Index> clone() const override { return std::make_shared<FailingIndex>(inner->clone(), fail); }
    void insert(const Value& v, RowLocation c) override {
        if (*fail) throw Error(ErrorCode::type, "Injected index insert failure");
        inner->insert(v, c);
    }
    void erase(const Value& v, RowLocation c) override {
        inner->erase(v, c);
        if (*fail) throw Error(ErrorCode::type, "Injected index erase failure");
    }
};
}
int main() { return tests([] {
    // Sharing immutable descriptors keeps copies alive without duplicating parameters.
    Value retained = std::int64_t{0};
    { Type t{"test.large.descriptor", 1, Bytes(4096, std::byte{3})};
      Value original = Opaque(t, Bytes(4096, std::byte{4})); retained = original;
      CHECK(&std::get<Opaque>(retained).type() == &std::get<Opaque>(original).type()); }
    CHECK(std::get<Opaque>(retained).type().parameters.size() == 4096);
    CHECK(std::get<Opaque>(retained).bytes()[4095] == std::byte{4});
    // Scalars really are registered add-ons; no implicit fallback in the engine.
    Registry empty(false);
    expect(ErrorCode::type, [&] { empty.validate(integer()); });
    install_scalar_types(empty);
    CHECK(empty.equal(std::int64_t{3}, std::int64_t{3}));
    Registry custom(false);
    auto scalar = empty.addon(integer());
    auto comparisons = std::make_shared<int>(0);
    auto eq = scalar.equal;
    scalar.equal = [eq, comparisons](ByteView p, const Value& a, const Value& b) { ++*comparisons; return eq(p, a, b); };
    custom.add(std::move(scalar));
    Database native(custom);
    { auto t = native.begin(); t.create_table("n", {{"id", integer(), true}}); t.insert("n", {std::int64_t{1}}); t.commit(); }
    CHECK(native.query(Query{"n", {}, key(1)}).rows.size() == 1); CHECK(*comparisons == 0);
    // Exact primary lookup needs no residual equality call; scans use registration.
    CHECK(native.query(Query{"n", {}, any_of({key(1)})}).rows.size() == 1); CHECK(*comparisons > 0);
    expect(ErrorCode::type, [&] { auto t = native.begin(); t.create_table("s", {{"v", text()}}); });

    Registry registry; timestamps::install(registry); spatial::install(registry);
    auto custom_factory_calls = std::make_shared<int>(0);
    registry.add(IndexAddon{"test.unique", [custom_factory_calls](const Type& t, const TypeAddon& a) {
        ++*custom_factory_calls; return make_hash_index(t, a);
    }, true});
    { Database db(registry); auto t = db.begin();
      t.create_table("custom", {{"id", timestamps::type(), true, "test.unique"}});
      t.insert("custom", {timestamps::value(4)});
      CHECK(t.query(Query{"custom", {}, Predicate{column("id"), Compare::equal, literal(timestamps::value(4))}}).rows.size() == 1);
      expect(ErrorCode::unsupported, [&] { t.create_table("bad", {{"b", spatial::type(), true, spatial::index_id}}); });
      CHECK(*custom_factory_calls == 1); }
    Database times(registry);
    { auto t = times.begin(); t.create_table("events", {{"at", timestamps::type(), true}});
      for (std::int64_t i = -300; i < 300; ++i) t.insert("events", {timestamps::value(i)}); t.commit(); }
    Query event{"events", {}, Predicate{column("at"), Compare::equal, literal(timestamps::value(-200))}};
    detail::visited_chunks() = 0;
    CHECK(times.query(event).rows.size() == 1); CHECK(detail::visited_chunks() == 1);
    auto old_times = times.begin();
    { auto t = times.begin();
      expect(ErrorCode::constraint, [&] { t.insert("events", {timestamps::value(-200)}); });
      expect(ErrorCode::constraint, [&] { t.update("events", {{"at", literal(timestamps::value(0))}}); });
      CHECK(t.query(event).rows.size() == 1);
      CHECK(t.erase("events", event.where) == 1); t.insert("events", {timestamps::value(900)}); t.commit(); }
    CHECK(times.query(event).rows.empty()); CHECK(old_times.query(event).rows.size() == 1);
    Registry swaps = registry;
    swaps.add(Function{"negate.timestamp", [](std::span<const Type>) { return timestamps::type(); },
        [](std::span<const Value> v) { return timestamps::value(-timestamps::microseconds(v[0])); }});
    Database swap_db(swaps);
    {auto t = swap_db.begin(); t.create_table("t", {{"v", timestamps::type(), true}});
     for (std::int64_t i = -200; i <= 200; ++i) t.insert("t", {timestamps::value(i)});
     t.update("t", {{"v", call("negate.timestamp", {column("v")})}}); t.commit();}
    CHECK(swap_db.query(Query{"t"}).rows.size() == 401);

    TempDirectory temp;
    const auto path = temp.path / "spatial";
    std::vector<spatial::Box> boxes;
    {
        auto db = Database::open(path, registry);
        auto t = db.begin();
        t.create_table("objects", {{"id", integer(), true}, {"box", spatial::type(), false, spatial::index_id}});
        std::mt19937 random(42);
        for (std::int64_t i = 0; i < 700; ++i) {
            double x = double(random() % 1000), y = double(random() % 1000);
            boxes.push_back({x, y, x + double(random() % 20), y + double(random() % 20)});
            t.insert("objects", {i, spatial::value(boxes.back())});
        }
        // Duplicate boxes are distinct rows, including in the same chunk.
        boxes.push_back(boxes[0]); t.insert("objects", {std::int64_t{700}, spatial::value(boxes[0])});
        t.commit();
        for (int i = 0; i < 100; ++i) {
            double x = double(random() % 1000), y = double(random() % 1000);
            spatial::Box b{x, y, x + 100, y + 100};
            std::vector<std::int64_t> expected;
            for (std::size_t j = 0; j < boxes.size(); ++j) if (spatial::overlaps(boxes[j], b)) expected.push_back(static_cast<std::int64_t>(j));
            auto result = db.query(nearby(b)); CHECK(result.rows.size() == expected.size());
            for (std::size_t j = 0; j < expected.size(); ++j) CHECK(std::get<std::int64_t>(result.rows[j][0]) == expected[j]);
        }
        auto snapshot = db.begin(); auto before = snapshot.query(nearby(boxes[0])).rows.size();
        { auto edit = db.begin(); edit.erase("objects", key(0)); edit.commit(); }
        CHECK(db.query(nearby(boxes[0])).rows.size() == before - 1);
        CHECK(snapshot.query(nearby(boxes[0])).rows.size() == before);
        spatial::Box remote{-100, -100, -90, -90};
        { auto edit = db.begin(); edit.update("objects", {{"box", literal(spatial::value(remote))}}, key(700));
          CHECK(edit.query(nearby(remote)).rows.size() == 1); edit.rollback(); }
        CHECK(db.query(nearby(remote)).rows.empty());
        { auto edit = db.begin(); edit.update("objects", {{"box", literal(spatial::value(remote))}}, key(700)); edit.commit(); }
        detail::visited_chunks() = 0;
        CHECK(db.query(nearby(remote)).rows.size() == 1); CHECK(detail::visited_chunks() == 1);
        auto invalid = nearby(remote); invalid.search->operation = "unknown"; invalid.limit = 0;
        expect(ErrorCode::unsupported, [&] { db.query(invalid); });
        invalid = nearby(remote); invalid.search->value = std::int64_t{3}; invalid.limit = 0;
        expect(ErrorCode::type, [&] { db.query(invalid); });
        expect(ErrorCode::unsupported, [&] { db.query(Query{"objects", {}, {}, {Order{column("box")}}}); });
        db.save(temp.path / "export"); db.checkpoint();
        auto edit = db.begin(); edit.insert("objects", {std::int64_t{701}, spatial::value(remote)}); edit.commit();
    }
    {
        auto loaded = Database::load(temp.path / "export", registry);
        CHECK(loaded.query(nearby({-100, -100, -90, -90})).rows.size() == 1);
        auto db = Database::open(path, registry);
        CHECK(db.query(nearby({-100, -100, -90, -90})).rows.size() == 2);
        auto snapshot = db.begin(); auto edit = db.begin(); edit.erase("objects");
        CHECK(edit.query(nearby({-100, -100, -90, -90})).rows.empty());
        edit.insert("objects", {std::int64_t{1}, spatial::value({0, 0, 1, 1})}); edit.commit();
        CHECK(snapshot.query(nearby({-100, -100, -90, -90})).rows.size() == 2);
    }
    { auto db = Database::open(path, registry); CHECK(db.query(nearby({0, 0, 1, 1})).rows.size() == 1); }
    expect(ErrorCode::type, [&] { Database::open(path); });
    { auto db = Database::open(temp.path / "timestamp-log", registry); auto t = db.begin();
      t.create_table("events", {{"at", timestamps::type(), true}});
      t.insert("events", {timestamps::value(-200)}); t.commit(); }
    CHECK(Database::open(temp.path / "timestamp-log", registry).query(event).rows.size() == 1);
    times.save(temp.path / "times");
    CHECK(Database::load(temp.path / "times", registry).query(event).rows.empty());

    // Equality need not imply equivalence for every secondary search operator.
    // A deliberately coarse equality must not suppress spatial maintenance.
    { Registry coarse;
      auto box_type = registry.addon(spatial::type());
      box_type.equal = [](ByteView, const Value&, const Value&) { return true; };
      coarse.add(std::move(box_type));
      auto factories = std::make_shared<Registry>(registry);
      coarse.add(IndexAddon{spatial::index_id, [factories](const Type& t, const TypeAddon&) { return factories->index(spatial::index_id, t); }});
      Database db(coarse); auto t = db.begin();
      t.create_table("objects", {{"id", integer(), true}, {"box", spatial::type(), false, spatial::index_id}});
      t.insert("objects", {std::int64_t{1}, spatial::value({0, 0, 1, 1})});
      t.update("objects", {{"box", literal(spatial::value({10, 10, 11, 11}))}}); t.commit();
      CHECK(db.query(nearby({0, 0, 1, 1})).rows.empty());
      CHECK(db.query(nearby({10, 10, 11, 11})).rows.size() == 1); }

    auto fail = std::make_shared<bool>(false);
    registry.add(IndexAddon{"test.fail", [fail](const Type& t, const TypeAddon& a) {
        return std::make_shared<FailingIndex>(make_hash_index(t, a), fail);
    }});
    Database faults(registry);
    {auto t = faults.begin(); t.create_table("f", {{"id", integer(), true}, {"b", spatial::type(), false, spatial::index_id}, {"n", integer(), false, "test.fail"}});
     t.insert("f", {std::int64_t{1}, spatial::value({0, 0, 1, 1}), std::int64_t{1}}); t.commit();}
    auto t = faults.begin(); *fail = true;
    expect(ErrorCode::type, [&] { t.insert("f", {std::int64_t{2}, spatial::value({2, 2, 3, 3}), std::int64_t{2}}); });
    expect(ErrorCode::type, [&] { t.update("f", {{"id", literal(std::int64_t{2})}, {"b", literal(spatial::value({2, 2, 3, 3}))}, {"n", literal(std::int64_t{2})}}); });
    expect(ErrorCode::type, [&] { t.erase("f", key(1)); });
    CHECK(t.query(Query{"f", {}, key(1)}).rows.size() == 1);
    Query q{"f"}; q.search = IndexSearch{"b", "overlaps", spatial::value({0, 0, 1, 1})};
    CHECK(t.query(q).rows.size() == 1);
    q.search->value = spatial::value({2, 2, 3, 3}); CHECK(t.query(q).rows.empty());
    *fail = false; t.insert("f", {std::int64_t{2}, spatial::value({2, 2, 3, 3}), std::int64_t{2}}); t.commit();
    CHECK(faults.query(q).rows.size() == 1);
}); }
