#pragma once
#include "bound.hpp"
#include "table_subset.hpp"
#include "scan.hpp"
#include "index.hpp"
#include <unordered_map>

namespace coresql::detail::execution {
// A statement-local candidate index for a leading integer equality against a
// captured parameter. NULL keys remain candidates because UNKNOWN can reach a
// later failing predicate. The complete WHERE is evaluated on the candidate rows.
class CorrelatedSource {
    std::shared_ptr<Table> table;
    std::size_t column, parameter;
    bool built = false;
    std::unordered_map<std::int64_t, std::vector<RowLocation>> keys;
    std::vector<RowLocation> nulls;

public:
    CorrelatedSource(std::shared_ptr<Table> t, std::size_t c, std::size_t p)
        : table(std::move(t)), column(c), parameter(p) {}
    std::shared_ptr<Table> subset(std::span<const Value> values) {
        if (is_null(values[parameter]))
            return {}; // Every row has UNKNOWN equality, so keep the original scan.
        if (!built) {
            visit_table_rows(*table, [&](auto location, const Row& row, auto) {
                const auto& value = row[column];
                if (is_null(value))
                    nulls.push_back(location);
                else
                    keys[std::get<std::int64_t>(value)].push_back(location);
#ifdef CORESQL_TESTING
                if (auto* counters = active_query_counters)
                    ++counters->correlation_index_rows;
#endif
                return true;
            });
            built = true;
        }
        auto found = keys.find(std::get<std::int64_t>(values[parameter]));
        std::vector<RowLocation> selected = nulls;
        if (found != keys.end())
            selected.insert(selected.end(), found->second.begin(), found->second.end());
        if (selected.size() > table->row_count / 2)
            return {};
        std::sort(selected.begin(), selected.end());
        return table_subset(*table, selected);
    }
};
inline std::shared_ptr<CorrelatedSource> correlated_source(const Tables& tables, const Query& query,
                                                           const std::vector<std::string>& parameters) {
    if (!query.repeatable || query.table.empty() || !query.where || query.join || !query.joins.empty() ||
        !query.relations.empty() || !query.compounds.empty() || query.source_where || query.search)
        return {};
    const auto* first = &*query.where;
    while (first->kind == Predicate::Kind::all && !first->children.empty())
        first = &first->children.front();
    if (first->kind != Predicate::Kind::comparison || first->operation != Compare::equal)
        return {};
    auto left = first->left, right = first->right;
    if (left.kind == Expr::Kind::parameter)
        std::swap(left, right);
    if (left.kind != Expr::Kind::column || right.kind != Expr::Kind::parameter)
        return {};
    const auto parameter = std::find(parameters.begin(), parameters.end(), right.name);
    if (parameter == parameters.end())
        return {};
    auto table = require_table(tables, query.table);
    Scope scope(*table);
    scope.left_alias = query.alias;
    auto [column, type] = scope.resolve(left);
    if (type != integer())
        return {};
    // Existing equality indexes already provide candidates without building a
    // duplicate query-local index or replacing their source with a private view.
    if ((table->primary && table->primary->column == column) ||
        std::any_of(table->ordered.begin(), table->ordered.end(),
                    [&](const auto& index) { return index->columns.front() == column; }))
        return {};
    return std::make_shared<CorrelatedSource>(table, column,
                                              static_cast<std::size_t>(parameter - parameters.begin()));
}

// Cache only successful results of explicitly repeatable queries with integer
// captures. Clearing at the capacity bounds entry count without retaining state
// beyond this bound expression or snapshot.
class SubqueryCache {
    using Key = std::vector<std::optional<std::int64_t>>;
    std::map<Key, Value> entries;

public:
    template <class Evaluate> Value get(std::span<const Value> values, Evaluate evaluate) {
        Key key;
        key.reserve(values.size());
        for (const auto& value : values)
            key.push_back(is_null(value) ? std::optional<std::int64_t>{} : std::get<std::int64_t>(value));
        if (auto found = entries.find(key); found != entries.end()) {
#ifdef CORESQL_TESTING
            if (auto* counters = active_query_counters)
                ++counters->subquery_cache_hits;
#endif
            return found->second;
        }
        auto result = evaluate();
        if (entries.size() == 4096)
            entries.clear();
        entries.emplace(std::move(key), result);
        return result;
    }
};
} // namespace coresql::detail::execution
