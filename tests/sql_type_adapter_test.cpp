#include "check.hpp"
#include "../sql/value_policy.hpp"
#include "coresql/sql.hpp"
#include <array>

using namespace coresql;
namespace {
Type tag(unsigned width = 16) {
    if (!width || width > 64)
        throw Error(ErrorCode::type, "Invalid tag width");
    return {"test.tag", 1, {std::byte(width)}};
}
std::string contents(const Value& v) {
    const auto& bytes = std::get<Opaque>(v).bytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
Value value(std::string_view text, Type t = tag()) {
    if (text.size() > std::to_integer<unsigned>(t.parameters[0]))
        throw Error(ErrorCode::type, "Tag too long");
    auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    return Opaque(std::move(t), Bytes(bytes.begin(), bytes.end()));
}
sql::SqlTypeAdapter adapter() {
    sql::SqlTypeAdapter a;
    a.type_id = "test.tag";
    a.names = {"tag", "label"};
    a.declaration = [](const sql::SqlDeclaration& declaration) {
        auto args = declaration.parameters;
        if (args.size() > 1 || (!args.empty() && args[0] > 64))
            throw Error(ErrorCode::type, "Tag takes one width");
        return tag(args.empty() ? 16 : static_cast<unsigned>(args[0]));
    };
    a.install = [](Registry& r) {
        r.add(EncodedTypeAddon{
            "test.tag", 1,
            [](ByteView p) {
                if (p.size() != 1)
                    throw Error(ErrorCode::type, "Bad tag type");
                (void)tag(std::to_integer<unsigned>(p[0]));
            },
            [](ByteView p, ByteView v) {
                if (v.size() > std::to_integer<unsigned>(p[0]))
                    throw Error(ErrorCode::type, "Tag too long");
            },
            [](ByteView, ByteView x, ByteView y) {
                return std::equal(x.begin(), x.end(), y.begin(), y.end());
            },
            [](ByteView, ByteView x, ByteView y) {
                return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end())   ? -1
                       : std::lexicographical_compare(y.begin(), y.end(), x.begin(), x.end()) ? 1
                                                                                              : 0;
            }});
        r.add(Function{
            "tag.concat",
            [](std::span<const Type> t) {
                if (t.size() != 2 || t[0].id != "test.tag" || t[0] != t[1])
                    throw Error(ErrorCode::type, "Tag types must match");
                return t[0];
            },
            [](std::span<const Value> v) { return value(contents(v[0]) + contents(v[1]), type_of(v[0])); }});
    };
    a.can_convert = [](const Type& s, const Type& t) {
        return (t.id == "test.tag" && (s == text() || s.id == "test.tag" || s.id == "sql.value")) ||
               (s.id == "test.tag" && t == text());
    };
    a.convert = [](const Value& v, const Type& t, bool) -> Value {
        auto s = type_of(v) == text() ? std::get<std::string>(v) : contents(v);
        return t == text() ? Value(s) : value(s, t);
    };
    a.typed_literal = [](std::string_view text) { return value(text); };
    a.operation = [](std::string_view op, std::span<const Type> types) -> std::optional<sql::SqlOperation> {
        if (op == "+" &&
            std::any_of(types.begin(), types.end(), [](const Type& t) { return t.id == "test.tag"; }))
            return sql::SqlOperation{"tag.concat"};
        return {};
    };
    a.common_type = [](std::span<const Type> types) -> std::optional<Type> {
        if (std::none_of(types.begin(), types.end(), [](const Type& t) { return t.id == "test.tag"; }))
            return {};
        unsigned width = 1;
        for (const auto& t : types) {
            if (t.id != "test.tag")
                throw Error(ErrorCode::type, "Mixed tag result");
            width = std::max(width, std::to_integer<unsigned>(t.parameters[0]));
        }
        return tag(width);
    };
    return a;
}
} // namespace
int main() {
    return tests([] {
        TempDirectory temp;
        auto types = sql::default_type_adapters();
        types.add(adapter());
        Registry registry;
        sql::install(registry, types);
        auto prepared = sql::Statement("INSERT INTO tags(id, name) VALUES (?, ?) RETURNING name", types);
        {
            auto db = Database::open(temp.path / "db", registry);
            sql::Connection c(db, registry, types);
            c.execute("CREATE TABLE tags(id INTEGER PRIMARY KEY, name LABEL(16))");
            auto rows = c.execute(prepared, std::array<Value, 2>{std::int64_t{1}, std::string("one")}).rows;
            CHECK(contents(rows[0][0]) == "one");
            c.execute("INSERT INTO tags VALUES(2, TAG 'two')");
            CHECK(std::get<std::string>(
                      c.execute("SELECT CAST(name AS TEXT) FROM tags WHERE name=TAG 'two'").rows[0][0]) ==
                  "two");
            CHECK(contents(c.execute("SELECT name+TAG '!' FROM tags WHERE id=1").rows[0][0]) == "one!");
            CHECK(is_null(c.execute("SELECT CAST(NULL AS TAG)").rows[0][0]));
            CHECK(type_of(c.execute("SELECT CAST(NULL AS TAG(8))").rows[0][0]) == tag(8));
            CHECK(contents(
                      c.execute("SELECT CASE WHEN 1 THEN CAST('a' AS TAG(8)) ELSE CAST('b' AS TAG(16)) END")
                          .rows[0][0]) == "a");
            auto compound = c.execute("SELECT CAST('a' AS TAG(8)) UNION ALL SELECT CAST('b' AS TAG(16))");
            CHECK(compound.rows.size() == 2 && type_of(compound.rows[0][0]) == tag());
            auto nested = c.execute("WITH x AS (SELECT name FROM tags) SELECT CAST(name AS TAG(8)) FROM x "
                                    "WHERE name IN (SELECT name FROM tags)");
            CHECK(nested.rows.size() == 2 && type_of(nested.rows[0][0]) == tag(8));
            CHECK(c.execute("SELECT id FROM tags a WHERE name=(SELECT name FROM tags b WHERE b.id=a.id)")
                      .rows.size() == 2);
            CHECK(
                contents(
                    c.execute("SELECT CASE WHEN 1 THEN TAG 'safe' ELSE CAST('0123456789abcdefg' AS TAG) END")
                        .rows[0][0]) == "safe");
            expect(ErrorCode::type,
                   [&] { c.execute("INSERT INTO tags VALUES(3,'ok'),(4,'0123456789abcdefg')"); });
            CHECK(c.execute("SELECT id FROM tags").rows.size() == 2);
            c.execute("BEGIN");
            c.execute("SAVEPOINT x");
            c.execute("UPDATE tags SET name=CAST('changed' AS TAG)");
            c.execute("ROLLBACK TO x");
            c.execute("RELEASE x");
            c.execute("COMMIT");
            CHECK(contents(c.execute("SELECT name FROM tags WHERE id=1").rows[0][0]) == "one");
            for (const auto& stmt : sql::prepare_script(
                     "INSERT INTO tags VALUES(3,'three');DELETE FROM tags WHERE id=3;", types))
                c.execute(stmt);
            expect(ErrorCode::type, [&] { c.execute("SELECT CAST('x' AS TAG(0))"); });
            expect(ErrorCode::type, [&] { c.execute(sql::Statement("SELECT 1")); });
            db.checkpoint();
            db.backup(temp.path / "backup");
        }
        for (const auto& file : {temp.path / "db", temp.path / "backup"}) {
            auto db = Database::open(file, registry);
            sql::Connection c(db, registry, types);
            CHECK(c.execute("SELECT name FROM tags").rows.size() == 2);
            db.begin().integrity_check();
        }
        // A copied registry is a stable snapshot; failed registration changes nothing.
        auto original = types;
        expect(ErrorCode::type, [&] { types.add(adapter()); });
        CHECK(types.same_configuration(original));
        auto duplicate = adapter();
        duplicate.type_id = "other";
        duplicate.names = {"TaG"};
        expect(ErrorCode::type, [&] { types.add(duplicate); });
        duplicate.names = {"INTEGER"};
        expect(ErrorCode::type, [&] { types.add(duplicate); });
        // Runtime conversion resolves claims once and still validates callback output.
        int probes = 0;
        auto counted = adapter();
        auto accepts = counted.can_convert;
        counted.can_convert = [&probes, accepts](const Type& source, const Type& target) {
            ++probes;
            return accepts(source, target);
        };
        sql::TypeAdapters counted_types(false);
        counted_types.add(counted);
        CHECK(sql::detail::convert(std::string("one"), tag(), false, counted_types) == value("one"));
        CHECK(probes == 1);
        CHECK(!counted_types.try_convert(std::int64_t{1}, tag(), false));
        expect(ErrorCode::type, [&] { counted_types.convert(std::int64_t{1}, tag(), false); });
        counted.convert = [](const Value&, const Type&, bool) -> Value { return std::int64_t{1}; };
        sql::TypeAdapters wrong_output(false);
        wrong_output.add(counted);
        expect(ErrorCode::type, [&] { wrong_output.try_convert(std::string("one"), tag(), false); });
        // Competing claims fail independently of registration order.
        auto first = adapter();
        first.type_id = "first";
        first.names = {"first"};
        auto second = adapter();
        second.type_id = "second";
        second.names = {"second"};
        first.operation = second.operation = [](std::string_view,
                                                std::span<const Type>) -> std::optional<sql::SqlOperation> {
            return sql::SqlOperation{"claim"};
        };
        first.common_type =
            second.common_type = [](std::span<const Type>) -> std::optional<Type> { return integer(); };
        first.can_convert = second.can_convert = [](const Type&, const Type&) { return true; };
        for (bool reversed : {false, true}) {
            sql::TypeAdapters ambiguous;
            ambiguous.add(reversed ? second : first);
            ambiguous.add(reversed ? first : second);
            expect(ErrorCode::type, [&] { ambiguous.operation("probe", {}); });
            expect(ErrorCode::type, [&] { ambiguous.common_type({}); });
            expect(ErrorCode::type, [&] { ambiguous.convertible(text(), integer()); });
            expect(ErrorCode::type, [&] { ambiguous.try_convert(std::string("1"), integer(), true); });
        }
        auto extra = adapter();
        extra.type_id = "extra";
        extra.names = {"extra"};
        types.add(extra);
        CHECK(!types.same_configuration(original));
        CHECK(!original.named("extra"));
        // Scalar-only configuration needs no optional domain adapters.
        sql::TypeAdapters empty;
        Registry scalars;
        sql::install(scalars, empty);
        Database db(scalars);
        sql::Connection c(db, scalars, empty);
        CHECK(std::get<std::int64_t>(c.execute("SELECT 1+2").rows[0][0]) == 3);
        expect(ErrorCode::unsupported, [&] { c.execute("CREATE TABLE t(d DATE)"); });
    });
}
