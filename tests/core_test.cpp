#include "check.hpp"
#include "coresql/encoding.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

using namespace coresql;

int main() { return tests([] {
    Database db;
    {
        auto tx = db.begin();
        tx.create_table("people", {{"id", integer()}, {"name", text()}, {"score", real()}});
        tx.insert("people", {std::int64_t{2}, std::string("beta"), 2.5});
        tx.insert("people", {std::int64_t{1}, std::string("alpha"), 1.5});
        expect(ErrorCode::schema, [&] { db.query(Query{"people"}); });
        CHECK(tx.query(Query{"people"}).rows.size() == 2);
        expect(ErrorCode::type, [&] { tx.insert("people", {1.0, std::string("bad"), 1.0}); });
        CHECK(tx.query(Query{"people"}).rows.size() == 2);
        tx.commit();
        expect(ErrorCode::state, [&] { tx.commit(); });
    }
    Query q{"people", {column("name")}, Predicate{column("score"), Compare::greater, literal(1.0)}, {Order{column("id")}}, 1};
    CHECK(std::get<std::string>(db.query(q).rows[0][0]) == "alpha");
    q.order_by[0].descending = true;
    CHECK(std::get<std::string>(db.query(q).rows[0][0]) == "beta");
    q.where = Predicate{column("id"), Compare::equal, literal(std::int64_t{1})};
    CHECK(db.query(q).rows.size() == 1);
    CHECK(db.stats().rows == 2);
    CHECK(db.stats().stored_payload_bytes == 41); // Two int64s, two doubles, nine text bytes.
    CHECK(db.stats().peak_stored_payload_bytes == 41);

    { auto tx = db.begin(); tx.insert("people", {std::int64_t{3}, std::string("discarded"), 0.0}); }
    CHECK(db.stats().rows == 2);
    auto first = db.begin(), stale = db.begin();
    first.insert("people", {std::int64_t{3}, std::string("gamma"), 3.5});
    first.commit();
    CHECK(stale.query(Query{"people"}).rows.size() == 2); // Stable snapshot.
    stale.insert("people", {std::int64_t{4}, std::string("delta"), 4.5});
    expect(ErrorCode::conflict, [&] { stale.commit(); });
    CHECK(db.stats().rows == 3);
    stale.rollback();
    expect(ErrorCode::state, [&] { stale.query(Query{"people"}); });

    auto tx = db.begin();
    tx.create_table("empty", {{"id", integer()}});
    expect(ErrorCode::schema, [&] { tx.create_table("duplicate", {{"id", integer()}, {"id", integer()}}); });
    expect(ErrorCode::type, [&] { tx.create_table("unknown", {{"v", {"absent", 1, {}}}}); });
    expect(ErrorCode::type, [&] { tx.create_table("invalid_builtin", {{"v", {"core.integer", 2, {}}}}); });
    {
        Registry locked;
        auto stolen = locked.addon(integer());
        stolen.layout = Layout::bytes;
        expect(ErrorCode::type, [&] { Registry other; other.add(stolen); });
        stolen = locked.addon(real());
        stolen.id = "test.rate";
        expect(ErrorCode::type, [&] { Registry other; other.add(stolen); });
    }
    expect(ErrorCode::schema, [&] { tx.query(Query{"empty", {column("missing")}, {}, {}, 0}); });
    expect(ErrorCode::type, [&] { tx.query(Query{"empty", {call("missing", {})}}); });
    expect(ErrorCode::type, [&] { tx.query(Query{"empty", {}, Predicate{column("id"), Compare::equal, literal(1.0)}}); });
    expect(ErrorCode::type, [&] { tx.query(Query{"empty", {literal(std::numeric_limits<double>::quiet_NaN())}}); });
    expect(ErrorCode::type, [&] { tx.insert("empty", {Opaque(integer(), {})}); });
    tx.commit();

    // No vector/timestamp library is linked. A test-only third type proves that
    // registration is open and even storage-only types work without ordering.
    Registry registry;
    registry.add(EncodedTypeAddon{"test.byte", 1,
        [](ByteView p) { if (!p.empty()) throw Error(ErrorCode::type, "No parameters"); },
        [](ByteView, ByteView b) { if (b.size() != 1) throw Error(ErrorCode::type, "One byte required"); }, {}, {}});
    registry.add(Function{"byte.read", [](std::span<const Type> args) {
        if (args.size() != 1 || args[0].id != "test.byte") throw Error(ErrorCode::type, "Expected byte");
        return integer();
    }, [](std::span<const Value> args) -> Value {
        return std::int64_t(std::to_integer<unsigned>(std::get<Opaque>(args[0]).bytes()[0]));
    }});
    registry.add(Function{"bad.result", [](std::span<const Type>) { return integer(); },
        [](std::span<const Value>) -> Value { return std::string("wrong"); }});
    Database extended(registry);
    auto e = extended.begin();
    Type byte{"test.byte", 1, {}};
    e.create_table("bytes", {{"v", byte}});
    e.insert("bytes", {Opaque(byte, {std::byte{42}})});
    e.commit();
    CHECK(std::get<std::int64_t>(extended.query(Query{"bytes", {call("byte.read", {column("v")})}}).rows[0][0]) == 42);
    expect(ErrorCode::unsupported, [&] { extended.query(Query{"bytes", {}, {}, {Order{column("v")}}}); });
    expect(ErrorCode::unsupported, [&] { extended.query(Query{"bytes", {}, Predicate{column("v"), Compare::equal, column("v")}}); });
    expect(ErrorCode::type, [&] { extended.query(Query{"bytes", {call("bad.result", {})}}); });
    registry.add(Function{"bad.real", [](std::span<const Type>) { return real(); },
        [](std::span<const Value>) -> Value { return std::numeric_limits<double>::quiet_NaN(); }});
    expect(ErrorCode::type, [&] { evaluate_constant(call("bad.real", {}), registry); });
    registry.add(Function{"bad.inf", [](std::span<const Type>) { return real(); },
        [](std::span<const Value>) -> Value { return std::numeric_limits<double>::infinity(); }});
    expect(ErrorCode::type, [&] { evaluate_constant(call("bad.inf", {}), registry); });
    {
        Registry restricted(false);
        auto integers = Registry{}.addon(integer());
        integers.native_ops = false;
        integers.validate_value = [](ByteView, const Value& v) {
            if (v == Value(std::int64_t{1}))
                throw Error(ErrorCode::type, "Integer 1 is rejected");
        };
        restricted.add(std::move(integers));
        restricted.add(Registry{}.addon(text()));
        restricted.add(Function{"always.true",
                                [](std::span<const Type> t) {
                                    if (t.size() != 2)
                                        throw Error(ErrorCode::type, "Expected two operands");
                                    return integer();
                                },
                                [](std::span<const Value>) -> Value { return std::int64_t{1}; }});
        expect(ErrorCode::type, [&] {
            evaluate_constant(
                call("always.true", {literal(std::int64_t{0}), literal(std::int64_t{0})}), restricted);
        });
        auto matched =
            choose(literal(std::int64_t{0}), "always.true",
                   {{literal(std::int64_t{0}), literal(std::string("matched"))}},
                   literal(std::string("fallback")));
        expect(ErrorCode::type, [&] { evaluate_constant(matched, restricted); });
    }

    TempDirectory temp;
    auto path = temp.path / "state.csql";
    db.save(path);
    expect(ErrorCode::io, [&] { db.save(path); }); // Never overwrite an export.
    auto restored = Database::load(path);
    CHECK(restored.stats().rows == 3);
    CHECK(restored.stats().tables == 2);
    CHECK(restored.stats().stored_payload_bytes == db.stats().stored_payload_bytes);
    CHECK(std::get<std::string>(restored.query(q).rows[0][0]) == "alpha");
    extended.save(temp.path / "extended.csql");
    expect(ErrorCode::type, [&] { Database::load(temp.path / "extended.csql"); });
    CHECK(Database::load(temp.path / "extended.csql", registry).stats().rows == 1);
    expect(ErrorCode::io, [&] { Database::load(temp.path / "absent"); });

    // Exercise framing checks with a valid checksum, not just the checksum gate.
    std::ifstream input(path, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(input)), {});
    auto write = [&](std::string bytes) {
        std::ofstream output(temp.path / "broken", std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    auto rehash = [](std::string& bytes) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t i = 0; i < bytes.size() - 8; ++i) { hash ^= static_cast<unsigned char>(bytes[i]); hash *= 1099511628211ULL; }
        for (unsigned i = 0; i < 8; ++i) bytes[bytes.size() - 8 + i] = static_cast<char>((hash >> (8 * i)) & 255);
    };
    auto corrupt = raw;
    corrupt[0] = static_cast<char>(255); rehash(corrupt); write(corrupt);
    expect(ErrorCode::format, [&] { Database::load(temp.path / "broken"); });
    corrupt = raw; for (std::size_t i = 16; i < 24; ++i) corrupt[i] = static_cast<char>(255);
    rehash(corrupt); write(corrupt);
    expect(ErrorCode::format, [&] { Database::load(temp.path / "broken"); });
    corrupt = raw; corrupt.insert(corrupt.end() - 8, 'x'); rehash(corrupt); write(corrupt);
    expect(ErrorCode::format, [&] { Database::load(temp.path / "broken"); });
    for (std::size_t n = 0; n < raw.size(); ++n) {
        write(raw.substr(0, n));
        expect(ErrorCode::format, [&] { Database::load(temp.path / "broken"); });
    }

    // Handles detect database destruction; moving a database keeps transactions valid.
    std::optional<Transaction> orphan;
    { Database parent; orphan.emplace(parent.begin()); }
    expect(ErrorCode::state, [&] { orphan->commit(); });
    Database parent;
    auto pending = parent.begin();
    Database moved(std::move(parent));
    pending.create_table("moved", {{"id", integer()}});
    pending.commit();
    CHECK(moved.stats().tables == 1);
    auto source = moved.begin();
    source.insert("moved", {std::int64_t{1}});
    auto target = moved.begin();
    target.insert("moved", {std::int64_t{2}});
    target = std::move(source); // Discards the destination's staged insert.
    expect(ErrorCode::state, [&] { source.commit(); });
    auto final = Transaction(std::move(target));
    expect(ErrorCode::state, [&] { target.commit(); });
    Database assigned;
    assigned = std::move(moved);
    final.commit();
    auto moved_rows = assigned.query(Query{"moved"}).rows;
    CHECK(moved_rows.size() == 1);
    CHECK(std::get<std::int64_t>(moved_rows[0][0]) == 1);

    {
        Type days{"test.days", 1, {}};
        Registry tagged;
        TypeAddon addon;
        addon.id = days.id;
        addon.layout = Layout::i64;
        addon.native_ops = true;
        addon.validate_type = [](ByteView p) { if (!p.empty()) throw Error(ErrorCode::type, "No parameters"); };
        addon.validate_value = [](ByteView, const Value& v) { (void)i64_payload(v); };
        addon.equal = [](ByteView, const Value& a, const Value& b) { return i64_payload(a) == i64_payload(b); };
        addon.compare = [](ByteView, const Value& a, const Value& b) {
            auto x = i64_payload(a), y = i64_payload(b);
            return (x > y) - (x < y);
        };
        addon.hash = [](ByteView, const Value& v) { return std::hash<std::int64_t>{}(i64_payload(v)); };
        tagged.add(std::move(addon));
        expect(ErrorCode::type, [&] { compact(integer(), std::int64_t{1}); });
        auto cell = compact(days, std::int64_t{7});
        CHECK(type_of(cell) == days && i64_payload(cell) == 7);
        CHECK(&std::get<Compact>(cell).type() == &intern(days));
        CHECK(&intern(days) == &intern(Type{days.id, days.version, days.parameters}));
        Database calendar(tagged);
        auto seed = calendar.begin();
        seed.create_table("events", {{"day", days, true}, {"n", integer()}});
        seed.create_table("other", {{"day", days, true}, {"n", integer()}});
        seed.insert("events", {compact(days, std::int64_t{1}), std::int64_t{10}});
        seed.insert("other", {compact(days, std::int64_t{1}), std::int64_t{20}});
        expect(ErrorCode::type, [&] { seed.insert("events", {std::int64_t{1}, std::int64_t{0}}); });
        seed.commit();
        Query join{"events"};
        join.alias = "a";
        join.join = Join{"other", "b", column("a", "day"), column("b", "day")};
        join.select = {column("a", "n"), column("b", "n")};
        auto joined = calendar.query(join);
        CHECK(joined.rows.size() == 1);
        CHECK(i64_payload(joined.rows[0][0]) == 10 && i64_payload(joined.rows[0][1]) == 20);
        Query grouped{"events", {column("day"), aggregate("count")}};
        grouped.group_by = {column("day")};
        grouped.repeatable = true;
        CHECK(i64_payload(calendar.query(grouped).rows[0][1]) == 1);
        tagged.add(Function{"wide.days", [days](std::span<const Type>) { return days; },
            [days](std::span<const Value>) -> Value { return compact(days, std::array<std::byte, 16>{}); }});
        expect(ErrorCode::type, [&] { evaluate_constant(call("wide.days", {}), tagged); });
        auto marker = compact(days, std::int64_t{0x0123456789ABCDEF});
        auto mark = calendar.begin();
        mark.create_table("markers", {{"day", days, true}});
        mark.insert("markers", {marker});
        mark.commit();
        auto compact_path = temp.path / "compact.csql";
        calendar.save(compact_path);
        auto compact_restored = Database::load(compact_path, tagged);
        CHECK(type_of(compact_restored.query(Query{"events"}).rows[0][0]) == days);
        CHECK(i64_payload(compact_restored.query(Query{"events"}).rows[0][0]) == 1);
        CHECK(i64_payload(compact_restored.query(Query{"markers"}).rows[0][0]) == 0x0123456789ABCDEF);
        std::ifstream compact_file(compact_path, std::ios::binary);
        std::string compact_bytes((std::istreambuf_iterator<char>(compact_file)), {});
        const char le[] = {'\xEF', '\xCD', '\xAB', '\x89', '\x67', '\x45', '\x23', '\x01'};
        const char be[] = {'\x01', '\x23', '\x45', '\x67', '\x89', '\xAB', '\xCD', '\xEF'};
        CHECK(compact_bytes.find(std::string(le, 8)) != std::string::npos);
        CHECK(compact_bytes.find(std::string(be, 8)) == std::string::npos);
    }
}); }
