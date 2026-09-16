#include "check.hpp"
#include "coresql/json.hpp"
#include "coresql/spatial.hpp"
#include "../src/key_index.hpp"
#include "../src/ordered_index.hpp"
#include "../src/query.hpp"

using namespace coresql;
namespace {
void check_analyze() {
    Database db;
    auto tx = db.begin();
    tx.create_table("keys", {{"a", integer(), false, {}, {}, true}, {"b", text(), false, {}, {}, true}});
    tx.create_index("keys", {"ab", {"a", "b"}, false, {true, false}});
    tx.create_index("keys", {"a", {"a"}});
    CHECK(tx.analyze().at("keys.ab.distinct") == 0);
    const Value one = std::int64_t{1}, two = std::int64_t{2};
    for (const auto& row : std::vector<Row>{{one, std::string("x")},
                                            {one, std::string("x")},
                                            {one, std::string("y")},
                                            {Null(integer()), std::string("x")},
                                            {Null(integer()), std::string("x")},
                                            {two, Null(text())}})
        tx.insert("keys", row);
    const auto counts = tx.analyze();
    CHECK(counts.at("keys.rows") == 6);
    CHECK(counts.at("keys.ab.distinct") == 4);
    CHECK(counts.at("keys.a.distinct") == 3);
    Query distinct{"keys", {column("a"), column("b")}};
    distinct.distinct = true;
    CHECK(tx.query(distinct).rows.size() == counts.at("keys.ab.distinct"));
    tx.commit();
    auto snapshot = db.begin();
    tx = db.begin();
    tx.erase("keys");
    CHECK(tx.analyze().at("keys.ab.distinct") == 0);
    CHECK(snapshot.analyze() == counts);
}

void check_index(const std::shared_ptr<Index>& index, const Value& a, const Value& b) {
    std::vector<IndexEntry> source{{a, {0, 0}}, {b, {0, 1}}};
    index->validate({});
    index->insert(a, {0, 0});
    index->insert(b, {0, 1});
    index->validate(source);
    auto wrong_values = source;
    std::swap(wrong_values[0].value, wrong_values[1].value);
    expect(ErrorCode::state, [&] { index->validate(wrong_values); });
    auto snapshot = index->clone();
    index->erase(a, {0, 0});
    expect(ErrorCode::state, [&] { index->validate(source); });
    index->insert(a, {99, 0});
    expect(ErrorCode::state, [&] { index->validate(source); });
    index->erase(a, {99, 0});
    index->insert(a, {0, 0});
    index->validate(source);
    index->insert(b, {0, 2});
    expect(ErrorCode::state, [&] { index->validate(source); });
    snapshot->validate(source);
}
struct UncheckedIndex : Index {
    std::shared_ptr<Index> clone() const override { return std::make_shared<UncheckedIndex>(*this); }
    void insert(const Value&, RowLocation) override {}
    void erase(const Value&, RowLocation) override {}
};
} // namespace
int main() {
    return tests([] {
        check_analyze();
        Registry registry;
        spatial::install(registry);
        json::install(registry);
        auto hash = make_hash_index(integer(), registry.addon(integer()));
        Value one = std::int64_t{1};
        hash->insert(one, {0, 0});
        auto snapshot = hash->clone();
        expect(ErrorCode::state, [&] { hash->erase(one, {0, 1}); });
        expect(ErrorCode::state, [&] { hash->erase(std::int64_t{2}, {0, 0}); });
        CHECK(hash->lookup(one) == (RowLocation{0, 0}));
        hash->validate(std::vector<IndexEntry>{{one, {0, 0}}});
        expect(ErrorCode::state, [&] { hash->validate({}); });
        hash->erase(one, {0, 0});
        CHECK(snapshot->lookup(one) == (RowLocation{0, 0}));
        check_index(registry.index(spatial::index_id, spatial::type()), spatial::value({0, 0, 1, 1}),
                    spatial::value({2, 2, 3, 3}));
        auto extractor = json::property("test.name", "/name");
        check_index(detail::make_key_index(extractor, registry.addon(text())), json::value(R"({"name":"a"})"),
                    json::value(R"({"name":"b"})"));
        // Duplicate emissions and rows with no extracted keys are valid.
        auto repeated = extractor;
        repeated.extract = [](const Value&) {
            return std::vector<Value>{std::string("a"), std::string("a")};
        };
        auto keys = detail::make_key_index(repeated, registry.addon(text()));
        keys->insert(one, {4, 130});
        keys->validate(std::vector<IndexEntry>{{one, {4, 130}}});
        auto empty = detail::make_key_index(extractor, registry.addon(text()));
        auto no_key = json::value("{}");
        empty->insert(no_key, {0, 0});
        empty->validate(std::vector<IndexEntry>{{no_key, {0, 0}}});
        // Exercise the public integrity dispatch and preservation of snapshots.
        registry.add(extractor);
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("boxes", {{"b", spatial::type(), false, spatial::index_id}});
        tx.create_table("docs", {{"doc", json::type(), false, "test.name"}});
        tx.insert("docs", {json::value(R"({"name":"a"})")});
        tx.insert("boxes", {spatial::value({0, 0, 1, 1})});
        tx.integrity_check();
        tx.commit();
        auto old = db.begin();
        tx = db.begin();
        tx.erase("boxes");
        tx.integrity_check();
        old.integrity_check();
        registry.add(IndexAddon{
            "unchecked", [](const Type&, const TypeAddon&) { return std::make_shared<UncheckedIndex>(); }});
        Database unsupported(registry);
        auto u = unsupported.begin();
        u.create_table("t", {{"v", integer(), false, "unchecked"}});
        u.insert("t", {one});
        expect(ErrorCode::unsupported, [&] { u.integrity_check(); });
        CHECK(u.query(Query{"t"}).rows.size() == 1);
        // Inject stale internal ordered-index locations: both chunk and slot failures.
        auto table = std::make_shared<detail::Table>();
        table->columns = {{"id", integer()}};
        auto chunk = std::make_shared<detail::Chunk>();
        chunk->rows = {{one}};
        chunk->rowids = {1};
        table->chunks.emplace(0, chunk);
        table->row_count = 1;
        auto ordered =
            std::make_shared<detail::OrderedIndex>(IndexDefinition{"id", {"id"}}, table->columns, registry);
        table->ordered.push_back(ordered);
        detail::Tables tables{{"t", table}};
        Query join{"t", {column("a", "id")}};
        join.alias = "a";
        join.join = Join{"t", "b", column("a", "id"), column("b", "id")};
        for (RowLocation bad : {RowLocation{99, 0}, RowLocation{0, 9}}) {
            ordered->insert(Row{one}, bad);
            expect(ErrorCode::state, [&] { detail::execution::run(tables, join, registry); });
            ordered->erase(Row{one}, bad);
        }
        ordered->insert(Row{one}, {0, 0});
        CHECK(detail::execution::run(tables, join, registry).rows == std::vector<Row>{{one}});
        // Scalar subqueries deliberately select the first row after ORDER BY.
        Query inner{"t", {column("v")}, {}, {Order{column("v"), true}}};
        Database scalars;
        auto s = scalars.begin();
        s.create_table("t", {{"v", integer()}});
        s.insert("t", {std::int64_t{1}});
        s.insert("t", {std::int64_t{2}});
        s.commit();
        CHECK(scalars.query(Query{"", {scalar_subquery(inner)}}).rows == std::vector<Row>{{std::int64_t{2}}});
        inner.limit = 0;
        CHECK(is_null(scalars.query(Query{"", {scalar_subquery(inner)}}).rows[0][0]));
    });
}
