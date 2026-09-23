#pragma once
#include "join_prefix.hpp"
#include "table_subset.hpp"
#include "scan.hpp"

namespace coresql::detail::execution {
// Apply proven non-failing local guards before building joins. Private names
// preserve full original sources for nested subqueries and repeated aliases.
inline std::optional<std::pair<Tables, Query>> prepare_join_inputs(const Tables& tables, const Query& query,
                                                                   const Registry& registry) {
    // Input views would move final-WHERE checks before source_where evaluation.
    if (query.source_where)
        return {};
    std::vector<std::vector<Predicate>> prefixes;
    if (query.where && query.where->kind == Predicate::Kind::any) {
        // Every OR arm needs a safe local guard. A row can be discarded only
        // when all arms are FALSE; UNKNOWN must still reach the full WHERE.
        for (const auto& branch : query.where->children) {
            auto arm = query;
            arm.where = branch;
            prefixes.push_back(native_join_prefix(tables, arm, registry));
        }
    } else
        prefixes.push_back(native_join_prefix(tables, query, registry));
    if (prefixes.empty() ||
        std::any_of(prefixes.begin(), prefixes.end(), [](const auto& prefix) { return prefix.empty(); }))
        return {};
    auto working = tables;
    auto next = query;
    bool changed = false;
    auto filter = [&](std::string& name, const std::string& alias) {
        std::vector<Predicate> alternatives;
        for (const auto& prefix : prefixes) {
            auto local = available_join_prefix(prefix, [&](const Expr& e) { return e.qualifier == alias; });
            if (!local)
                return; // This arm may still succeed for any row of this input.
            alternatives.push_back(std::move(*local));
        }
        auto early =
            alternatives.size() == 1 ? std::move(alternatives.front()) : any_of(std::move(alternatives));
        auto table = require_table(tables, name);
        Scope scope(*table);
        scope.left_alias = alias;
        auto predicate = bind_predicate(early, scope, registry);
        auto candidates = join_guard_candidates(*table, *predicate, registry);
        QueryBuffer candidate_memory;
        if (candidates)
            candidate_memory.add(candidates->rows.size() * sizeof(RowLocation));
        std::vector<RowLocation> selected;
        visit_chunks(*table, candidates, [&](auto id, const auto& chunk, RowSelection rows) {
            return visit_rows(*chunk, rows, [&](auto position, const Row& row) {
#ifdef CORESQL_TESTING
                if (auto* counters = active_query_counters)
                    ++counters->rows_tested;
#endif
                if (predicate->truth(RowView(row, chunk->rowids[position]), registry) != 0)
                    selected.push_back({id, chunk->slot(position)});
                return true;
            });
        });
        if (selected.size() == table->row_count)
            return;
        name = "\x01input";
        while (working.contains(name))
            name += '_';
        working.emplace(name, table_subset(*table, selected));
        changed = true;
    };
    filter(next.table, next.alias);
    for (auto& join : next.joins)
        filter(join.table, join.alias);
    if (!changed)
        return {};
    return std::pair{std::move(working), std::move(next)};
}
} // namespace coresql::detail::execution
