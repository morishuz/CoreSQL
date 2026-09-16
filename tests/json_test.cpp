#include "check.hpp"
#include "coresql/json.hpp"
#include "../src/state.hpp"
#include <limits>
using namespace coresql;
namespace {
Registry registry() {
    Registry r; json::install(r);
    r.add(json::property("doc.name.v1", "/name"));
    r.add(json::property("doc.n.v1", "/n", integer()));
    return r;
}
Query named(std::string table, std::string name) {
    return {std::move(table), {column("id")}, contains(column("doc"), "doc.name.v1", literal(std::move(name)))};
}
std::vector<std::int64_t> ids(const Result& result) {
    std::vector<std::int64_t> out;
    for (const auto& row : result.rows) out.push_back(std::get<std::int64_t>(row[0]));
    return out;
}
void parser() {
    auto r = registry();
    auto extract = [&](std::string_view document, std::string_view pointer, Type key = text()) {
        return json::property("test", pointer, key).extract(json::value(document));
    };
    CHECK(std::get<std::string>(extract(R"({"a/b":{"~key":["zero","one"]}})", "/a~1b/~0key/1")[0]) == "one");
    CHECK(std::get<std::string>(extract(R"({"":"empty","01":"object"})", "/")[0]) == "empty");
    CHECK(std::get<std::string>(extract(R"({"01":"object"})", "/01")[0]) == "object");
    CHECK(extract(R"(["a","b"])", "/01").empty());
    CHECK(extract(R"(["a","b"])", "/-").empty());
    CHECK(std::get<std::string>(extract(R"("\uD83D\uDE80\u0000")", "")[0]) == std::string("🚀\0", 5));
    CHECK(std::get<std::string>(extract(R"({"\u0061":"é"})", "/a")[0]) == "é");
    CHECK(std::get<std::int64_t>(extract("9223372036854775807", "", integer())[0]) == std::numeric_limits<std::int64_t>::max());
    CHECK(std::get<std::int64_t>(extract("-9223372036854775808", "", integer())[0]) == std::numeric_limits<std::int64_t>::min());
    CHECK(extract("9223372036854775808", "", integer()).empty());
    CHECK(extract("1.0", "", integer()).empty());
    CHECK(std::get<double>(extract("1e2", "", real())[0]) == 100.0);
    CHECK(extract("1e999", "", real()).empty());
    for (auto doc : {"null", "true", "false", "{}", "[]", "42"}) CHECK(extract(doc, "").empty());
    CHECK(extract(R"({"other":"x"})", "/name").empty());
    for (auto doc : {"", " ", "01", "+1", "-", "1.", "1e", "[1,]", "{\"a\":}", "{}x", "nul", "[", "\"", "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\u0041\"", "\"\\x\"", "{\"a\":1,\"\\u0061\":2}"})
        expect(ErrorCode::type, [&] { json::value(doc); });
    for (const std::string doc : {std::string("\"\xc0\xaf\""), std::string("\"\xed\xa0\x80\""), std::string("\"\xf4\x90\x80\x80\""), std::string("\"\0\"", 3)})
        expect(ErrorCode::type, [&] { json::value(doc); });
    expect(ErrorCode::type, [&] { json::value(std::string(66, '[') + "0" + std::string(66, ']')); });
    for (auto path : {"name", "/~", "/~2"}) expect(ErrorCode::type, [&] { json::property("bad", path); });
    expect(ErrorCode::unsupported, [&] { json::property("bad", "", json::type()); });
    auto invalid = Opaque(json::type(), {std::byte{'!'}});
    expect(ErrorCode::type, [&] { r.validate(invalid, json::type()); });
}
void database() {
    Database db(registry()); auto tx = db.begin();
    for (auto name : {"scan", "indexed"}) {
        tx.create_table(name, {{"id", integer(), true}, {"doc", json::type(), false, std::string(name) == "indexed" ? "doc.name.v1" : ""}});
        for (std::int64_t i = 0; i < 600; ++i)
            tx.insert(name, {i, json::value("{\"name\":\"" + (i % 137 == 0 ? std::string("rare") : std::string("common")) + "\",\"n\":" + std::to_string(i) + "}")});
        tx.insert(name, {std::int64_t{600}, json::value("{}")});
    }
    tx.commit(); auto snapshot = db.begin();
    for (auto value : {"rare", "common", "absent"}) {
        auto expected = ids(db.query(named("scan", value)));
        detail::visited_chunks() = 0;
        CHECK(ids(db.query(named("indexed", value))) == expected);
        if (std::string(value) == "absent") CHECK(detail::visited_chunks() == 0);
    }
    Query explicit_search{"indexed", {column("id")}};
    explicit_search.search = IndexSearch{"doc", "equal", std::string("rare")};
    CHECK(ids(db.query(explicit_search)) == ids(db.query(named("scan", "rare"))));
    auto bad = named("indexed", "rare"); bad.where->right = literal(std::int64_t{1}); bad.limit = 0;
    expect(ErrorCode::type, [&] { db.query(bad); });
    bad = named("indexed", "rare"); bad.where->extractor = "missing";
    expect(ErrorCode::type, [&] { db.query(bad); });
    auto nested = named("indexed", "rare");
    nested.where = all_of({*nested.where, not_({column("id"), Compare::equal, literal(std::int64_t{0})})});
    CHECK(db.query(nested).rows.size() == 4);
    nested.where = any_of({*named("indexed", "rare").where, {column("id"), Compare::equal, literal(std::int64_t{600})}});
    CHECK(db.query(nested).rows.size() == 6);
    auto update = db.begin();
    for (auto name : {"scan", "indexed"}) {
        CHECK(update.update(name, {{"doc", literal(json::value(R"({"name":"new"})"))}}, named(name, "rare").where) == 5);
        CHECK(update.erase(name, named(name, "common").where) == 595);
    }
    CHECK(ids(update.query(named("scan", "new"))) == ids(update.query(named("indexed", "new"))));
    CHECK(snapshot.query(named("indexed", "rare")).rows.size() == 5);
    update.rollback(); CHECK(db.query(named("indexed", "rare")).rows.size() == 5);
    auto remove = db.begin();
    CHECK(remove.erase("indexed", Predicate{column("id"), Compare::equal, literal(std::int64_t{1})}) == 1);
    CHECK(remove.query(named("indexed", "common")).rows.size() == 594); // same-chunk counts survive
    remove.commit(); CHECK(snapshot.query(named("indexed", "common")).rows.size() == 595);
    auto clear = db.begin(); clear.erase("indexed"); clear.insert("indexed", {std::int64_t{9}, json::value(R"({"name":"rare"})")}); clear.commit();
    CHECK(ids(db.query(named("indexed", "rare"))) == std::vector<std::int64_t>{9});
}
void reuse_and_failure() {
    Registry r;
    auto fail = std::make_shared<bool>(false);
    // Deliberately no ordering for keys: equality + hashing must suffice.
    Type token{"test.token", 1, {}};
    TypeAddon addon; addon.id = token.id;
    addon.validate_type = [](ByteView) {};
    addon.validate_value = [](ByteView, const Value& v) { if (std::get<Opaque>(v).bytes().size() != 1) throw Error(ErrorCode::type, "bad token"); };
    addon.equal = [](ByteView, const Value& a, const Value& b) { return std::get<Opaque>(a).bytes()[0] == std::get<Opaque>(b).bytes()[0]; };
    addon.hash = [](ByteView, const Value&) { return std::size_t{0}; }; // exercise collisions
    r.add(addon);
    r.add(KeyExtractor{"characters.v1", text(), token, [fail, token](const Value& source) {
        if (*fail) throw Error(ErrorCode::type, "injected extraction failure");
        std::vector<Value> result;
        for (char c : std::get<std::string>(source)) result.emplace_back(Opaque(token, {static_cast<std::byte>(c)}));
        return result;
    }});
    auto predicate = contains(column("s"), "characters.v1", literal(Opaque(token, {std::byte{'a'}})));
    Query q{"t", {}, predicate}; Database db(r); auto tx = db.begin();
    tx.create_table("t", {{"id", integer(), true}, {"s", text(), false, "characters.v1"}});
    tx.insert("t", {std::int64_t{1}, std::string("aa")}); tx.insert("t", {std::int64_t{2}, std::string("ab")}); tx.commit();
    auto old = db.begin(); auto edit = db.begin();
    edit.erase("t", Predicate{column("id"), Compare::equal, literal(std::int64_t{1})});
    CHECK(edit.query(q).rows.size() == 1);
    *fail = true;
    expect(ErrorCode::type, [&] { edit.insert("t", {std::int64_t{3}, std::string("ac")}); });
    expect(ErrorCode::type, [&] { edit.update("t", {{"s", literal(std::string("z"))}}); });
    expect(ErrorCode::type, [&] { edit.erase("t", Predicate{column("id"), Compare::equal, literal(std::int64_t{2})}); });
    *fail = false;
    CHECK(edit.query(q).rows.size() == 1); CHECK(old.query(q).rows.size() == 2); edit.commit();
    CHECK(db.stats().rows == 1);
    // Wrong output types are rejected both by scans and by the shared index.
    r.add(KeyExtractor{"broken.v1", text(), integer(), [](const Value&) { return std::vector<Value>{std::string("wrong")}; }});
    Database broken(r); auto b = broken.begin(); b.create_table("t", {{"s", text()}}); b.insert("t", {std::string("x")});
    expect(ErrorCode::type, [&] { b.query({"t", {}, contains(column("s"), "broken.v1", literal(std::int64_t{1}))}); });
    b.create_table("i", {{"s", text(), false, "broken.v1"}});
    expect(ErrorCode::type, [&] { b.insert("i", {std::string("x")}); });
    CHECK(b.query({"i"}).rows.empty());
}
void atomic_key_edits() {
    Registry r;
    auto fail = std::make_shared<bool>(false);
    Type key{"test.key", 1, {}};
    TypeAddon addon; addon.id = key.id;
    addon.validate_type = [](ByteView) {};
    addon.validate_value = [](ByteView, const Value&) {};
    addon.equal = [](ByteView, const Value& a, const Value& b) { return std::get<Opaque>(a).bytes()[0] == std::get<Opaque>(b).bytes()[0]; };
    addon.hash = [fail](ByteView, const Value& value) {
        auto b = std::get<Opaque>(value).bytes()[0];
        if (*fail && b == std::byte{'z'}) throw Error(ErrorCode::type, "hash failed after first key");
        return std::to_integer<std::size_t>(b);
    };
    r.add(addon);
    r.add(KeyExtractor{"letters", text(), key, [key](const Value& source) {
        std::vector<Value> out; for (char c : std::get<std::string>(source)) out.emplace_back(Opaque(key, {static_cast<std::byte>(c)})); return out;
    }});
    auto index = r.index("letters", text()); index->insert(std::string("b"), {1, 0});
    auto snapshot = index->clone();
    *fail = true;
    expect(ErrorCode::type, [&] { index->insert(std::string("az"), {2, 0}); });
    *fail = false;
    CHECK(index->search("equal", Opaque(key, {std::byte{'a'}})).rows.empty());
    CHECK(index->search("equal", Opaque(key, {std::byte{'b'}})).rows == (std::vector<RowLocation>{{1, 0}}));
    index->insert(std::string("az"), {2, 0});
    CHECK(snapshot->search("equal", Opaque(key, {std::byte{'a'}})).rows.empty());
}
void persistence() {
    TempDirectory dir; auto file = dir.path / "db"; auto export_file = dir.path / "export";
    {
        auto db = Database::open(file, registry()); auto tx = db.begin();
        tx.create_table("indexed", {{"id", integer(), true}, {"doc", json::type(), false, "doc.name.v1"}});
        tx.insert("indexed", {std::int64_t{1}, json::value(R"({"name":"before"})")}); tx.commit(); db.checkpoint();
        auto edit = db.begin(); edit.update("indexed", {{"doc", literal(json::value(R"({"name":"after"})"))}}); edit.commit(); db.save(export_file);
    }
    for (bool exported : {false, true}) {
        auto db = exported ? Database::load(export_file, registry()) : Database::open(file, registry());
        CHECK(db.query(named("indexed", "before")).rows.empty());
        CHECK(ids(db.query(named("indexed", "after"))) == std::vector<std::int64_t>{1});
    }
    Registry missing; json::install(missing);
    expect(ErrorCode::type, [&] { Database::load(export_file, missing); });
}
}
int main() { return tests([] { parser(); database(); reuse_and_failure(); atomic_key_edits(); persistence(); }); }
