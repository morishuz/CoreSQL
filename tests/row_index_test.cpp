#include "check.hpp"
#include "coresql/json.hpp"
#include <algorithm>
#include <random>
using namespace coresql;
namespace {
Predicate id(std::int64_t value) { return {column("id"), Compare::equal, literal(value)}; }
Query name(std::string table, std::string value) {
    return {std::move(table), {column("id")}, contains(column("doc"), "name", literal(std::move(value)))};
}
std::vector<std::int64_t> ids(const Result& result) {
    std::vector<std::int64_t> out; for (const auto& row : result.rows) out.push_back(std::get<std::int64_t>(row[0])); return out;
}
struct Probe final : Index {
    std::vector<std::pair<RowLocation, Value>> entries;
    std::shared_ptr<int> checks;
    bool exact, invalid;
    Probe(std::shared_ptr<int> checks, bool exact, bool invalid) : checks(checks), exact(exact), invalid(invalid) {}
    std::shared_ptr<Index> clone() const override { return std::make_shared<Probe>(*this); }
    void insert(const Value& value, RowLocation location) override { entries.emplace_back(location, value); }
    void erase(const Value&, RowLocation location) override {
        auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == location; });
        CHECK(found != entries.end()); entries.erase(found);
    }
    void validate_search(const std::string& op) const override { if (op != "equal") throw Error(ErrorCode::unsupported, "unsupported"); }
    IndexResult search(const std::string& op, const Value& key) const override {
        validate_search(op); IndexResult result{{}, exact};
        if (invalid) return {{{0, 9999}}, true};
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            if (!exact || std::get<std::string>(it->second) == std::get<std::string>(key)) {
                result.rows.push_back(it->first); result.rows.push_back(it->first); // normalize order and duplicates
            }
        return result;
    }
    bool matches(const std::string&, const Value& source, const Value& key) const override {
        ++*checks;
        CHECK(!exact); // Exact search must bypass this callback.
        return std::get<std::string>(source) == std::get<std::string>(key);
    }
};
void result_contract() {
    Registry r; auto checks = std::make_shared<int>(0);
    for (const std::string kind : {"exact", "candidate", "invalid"})
        r.add(IndexAddon{kind, [kind, checks](const Type&, const TypeAddon&) { return std::make_shared<Probe>(checks, kind == "exact", kind == "invalid"); }});
    Database db(r); auto tx = db.begin();
    for (const std::string kind : {"exact", "candidate", "invalid"}) {
        tx.create_table(kind, {{"id", integer()}, {"s", text(), false, kind}});
        for (std::int64_t i = 0; i < 200; ++i) tx.insert(kind, {i, std::string(i % 2 ? "a" : "b")});
    }
    tx.commit();
    Query q{"exact", {column("id")}}; q.search = IndexSearch{"s", "equal", std::string("a")};
    auto exact = ids(db.query(q)); CHECK(exact.size() == 100); CHECK(*checks == 0);
    q.table = "candidate"; CHECK(ids(db.query(q)) == exact); CHECK(*checks == 200);
    q.table = "invalid"; expect(ErrorCode::state, [&] { db.query(q); });
    auto old = db.begin(); auto erase = db.begin(); erase.erase("exact", id(0)); erase.commit();
    q.table = "exact"; CHECK(ids(db.query(q)) == exact); CHECK(ids(old.query(q)) == exact);
    q.where = id(3); CHECK(ids(db.query(q)) == std::vector<std::int64_t>{3});
    q.where = id(2); CHECK(db.query(q).rows.empty()); // exact proof is not a proof of WHERE
}
void slot_boundaries() {
    Registry r;
    r.add(KeyExtractor{"duplicates", text(), text(), [](const Value& v) { return std::vector<Value>{v, v}; }});
    Database db(r); auto tx = db.begin();
    tx.create_table("t", {{"id", integer(), true}, {"s", text(), false, "duplicates"}});
    for (std::int64_t i = 0; i < 128; ++i) tx.insert("t", {i, std::string("a")});
    tx.commit(); auto old = db.begin();
    Query q{"t", {column("id")}, contains(column("s"), "duplicates", literal(std::string("a")))};
    for (std::int64_t i = 0; i < 140; ++i) {
        auto edit = db.begin(); CHECK(edit.erase("t", id(i)) == 1);
        edit.insert("t", {i + 128, std::string("a")}); edit.commit();
        auto result = ids(db.query(q)); CHECK(result.size() == 128);
        CHECK(result.front() == i + 1); CHECK(result.back() == i + 128);
        CHECK(db.query({"t", {}, id(i)}).rows.empty());
        CHECK(db.query({"t", {}, id(i + 128)}).rows.size() == 1);
    }
    auto result = ids(old.query(q)); CHECK(result.size() == 128); CHECK(result.front() == 0); CHECK(result.back() == 127);
}
void extraction_and_recovery() {
    auto calls = std::make_shared<int>(0);
    auto registry = [&] {
        Registry r; json::install(r);
        auto extractor = json::property("name", "/name"); auto extract = extractor.extract;
        extractor.extract = [calls, extract](const Value& value) { ++*calls; return extract(value); };
        r.add(std::move(extractor));
        r.add(json::property("number", "/n", integer()));
        r.add(Function{"fail", [](std::span<const Type>) { return integer(); }, [](std::span<const Value>) -> Value { throw Error(ErrorCode::state, "expected failure"); }});
        return r;
    };
    TempDirectory dir; auto path = dir.path / "rows"; auto exported = dir.path / "export";
    {
        auto db = Database::open(path, registry()); auto tx = db.begin();
        for (const std::string table : {"indexed", "scan"}) {
            tx.create_table(table, {{"id", integer(), true}, {"doc", json::type(), false, table == "indexed" ? "name" : ""}});
            for (std::int64_t i = 0; i < 400; ++i)
                tx.insert(table, {i, json::value("{\"name\":\"" + std::string(i == 199 ? "rare" : "common") + "\",\"n\":" + std::to_string(i) + "}")});
        }
        tx.commit(); db.checkpoint(); auto old = db.begin();
        *calls = 0; CHECK(db.query(name("indexed", "rare")).rows.size() == 1); CHECK(*calls == 0);
        CHECK(db.query(name("scan", "rare")).rows.size() == 1); CHECK(*calls == 400);
        auto q = name("indexed", "rare");
        q.where = all_of({*q.where, contains(column("doc"), "number", literal(std::int64_t{198}))});
        CHECK(db.query(q).rows.empty());
        Predicate fail{call("fail", {}), Compare::equal, literal(std::int64_t{0})};
        q.where = all_of({fail, *name("indexed", "absent").where});
        expect(ErrorCode::state, [&] { db.query(q); });
        q.where = all_of({*name("indexed", "absent").where, fail}); CHECK(db.query(q).rows.empty());
        q.where = all_of({*name("indexed", "rare").where, fail}); expect(ErrorCode::state, [&] { db.query(q); });
        // Repeated compaction, swaps in value membership, reinsertion and rollback.
        std::mt19937 random(37);
        for (int step = 0; step < 40; ++step) {
            auto edit = db.begin(); auto chosen = static_cast<std::int64_t>(random() % 400);
            for (const std::string table : {"scan", "indexed"}) {
                if (step % 2) edit.erase(table, id(chosen));
                else edit.update(table, {{"doc", literal(json::value(R"({"name":"rare","n":-1})"))}}, id(chosen));
                if (step % 7 == 0) edit.insert(table, {std::int64_t{1000 + step}, json::value(R"({"name":"common"})")});
            }
            for (const std::string value : {"rare", "common", "missing"})
                CHECK(ids(edit.query(name("scan", value))) == ids(edit.query(name("indexed", value))));
            if (step % 5 == 0) edit.rollback(); else edit.commit();
        }
        *calls = 0; CHECK(ids(old.query(name("indexed", "rare"))) == std::vector<std::int64_t>{199}); CHECK(*calls == 0);
        // Exactly one extraction for deletion; none for the surviving neighbours.
        CHECK(old.erase("indexed", id(199)) == 1); CHECK(*calls == 1); old.rollback();
        db.save(exported);
    }
    for (bool snapshot : {false, true}) {
        auto db = snapshot ? Database::load(exported, registry()) : Database::open(path, registry());
        for (const std::string value : {"rare", "common", "missing"}) {
            auto expected = ids(db.query(name("scan", value))); *calls = 0;
            CHECK(ids(db.query(name("indexed", value))) == expected); CHECK(*calls == 0);
        }
    }
}
}
int main() { return tests([] { result_contract(); slot_boundaries(); extraction_and_recovery(); }); }
