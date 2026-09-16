#include "check.hpp"
#include "../src/table_subset.hpp"
#include "../src/scan.hpp"
#include "../src/integer_join.hpp"
using namespace coresql;
using namespace coresql::detail;
using namespace coresql::detail::execution;
int main() {
    return tests([] {
        Registry registry;
        Tables tables;
        Result data{{integer(), text()}, {}};
        for (std::int64_t i = 0; i < 300; ++i)
            data.rows.push_back({i % 5, std::to_string(i) + std::string(2048, 'x')});
        auto original_name = materialize(tables, std::move(data));
        auto original = tables.at(original_name);
        // Nonidentity slots exercise logical row IDs and selection lookup.
        auto chunk = original->chunks.begin()->second;
        for (std::uint32_t i = 0; i < chunk->rows.size(); ++i)
            chunk->slots.push_back(i * 2);
        std::vector<RowLocation> selected{{0, 2}, {0, 12}, {1, 4}};
        const std::vector<RowLocation> missing{{999, 0}};
        expect(ErrorCode::state, [&] { table_subset(*original, missing); });
        auto view = table_subset(*original, selected);
        CHECK(view->chunks.begin()->second == chunk);
        CHECK(view->row_count == 3 && !view->primary && view->ordered.empty());
        std::vector<const Row*> addresses;
        visit_table_rows(*view, [&](auto, const Row& row, auto) {
            addresses.push_back(&row);
            return true;
        });
        CHECK(addresses.size() == 3 && addresses[0] == &chunk->rows[1]);
        Result copied{{integer(), text()}, {}};
        for (auto row : addresses)
            copied.rows.push_back(*row);
        auto copy_name = materialize(tables, std::move(copied));
        tables["view"] = view;
        Query q{"view", {column("0"), column("1")}};
        auto compare_view = [&](Query query) {
            auto expected = query;
            expected.table = copy_name;
            if (expected.join)
                expected.join->table = copy_name;
            for (auto& join : expected.joins)
                join.table = copy_name;
            expected.repeatable = false;
            CHECK(run(tables, query, registry).rows == run(tables, expected, registry).rows);
        };
        compare_view(q);
        q.order_by = {{column("1"), true}};
        q.limit = 2;
        compare_view(q);
        q = Query{"view", {aggregate("count"), aggregate("sum", {column("0")})}};
        q.repeatable = true;
        compare_view(q);
        q = Query{"view", {column("a", "1"), column("b", "1")}};
        q.alias = "a";
        q.join = Join{"view", "b", column("a", "0"), column("b", "0")};
        compare_view(q); // Integer hash candidates must exclude hidden rows.
        for (auto kind : {JoinKind::left, JoinKind::right, JoinKind::full}) {
            q.join->kind = kind;
            q.join->on = Predicate{column("a", "0"), Compare::equal, column("b", "0")};
            compare_view(q);
        }
        Query chain{"view", {column("a", "1"), column("b", "1"), column("c", "1")}};
        chain.alias = "a";
        chain.repeatable = true;
        Join first{"view", "b", column("a", "0"), column("b", "0")};
        first.kind = JoinKind::full;
        first.on = all_of({Predicate{column("a", "0"), Compare::equal, column("b", "0")},
                           Predicate{column("b", "0"), Compare::equal, literal(std::int64_t{1})}});
        Join second{"view", "c", column("b", "0"), column("c", "0")};
        second.kind = JoinKind::left;
        chain.joins = {first, second};
        compare_view(chain); // Retain hidden ON keys across NULL-extended intermediate stages.
        q.join->cross = true;
        q.join->on.reset();
        q.join->kind = JoinKind::inner;
        compare_view(q);
        auto empty = table_subset(*view, {});
        CHECK(empty->row_count == 0 && empty->chunks.empty());
        // Retained chunks keep borrowed rows alive after original table removal.
        tables.erase(original_name);
        original.reset();
        chunk.reset();
        CHECK(std::get<std::string>((*addresses[0])[1]).starts_with("1"));
        Query identity{"view", {row_id()}};
        auto ids = run(tables, identity, registry);
        CHECK(ids.rows.size() == 3);
        CHECK(ids.rows[0][0] != ids.rows[1][0]);
    });
}
