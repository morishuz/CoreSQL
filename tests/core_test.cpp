#include "check.hpp"
#include "coresql/encoding.hpp"
#include <algorithm>
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
        TypeAddon days;
        days.id = "test.days";
        days.version = 1;
        days.layout = Layout::i64;
        days.validate_type = [](ByteView p) { if (!p.empty()) throw Error(ErrorCode::type, "No parameters"); };
        days.validate_value = [](ByteView, const Value&) {};
        expect(ErrorCode::type, [&] { locked.add(days); });
        auto stolen = locked.addon(integer());
        stolen.id = "test.days";
        expect(ErrorCode::type, [&] { Registry other; other.add(stolen); });
        stolen = locked.addon(integer());
        stolen.layout = Layout::bytes;
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
}); }
