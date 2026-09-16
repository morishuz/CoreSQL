#include "bound.hpp"
#include <set>

namespace coresql::detail::execution {
std::optional<Query> prepare_membership_join(const Tables& tables, const Query& query,
                                             const Registry& registry) {
    if (!query.limit || query.group_by.empty() || !query.repeatable || query.source_where || !query.where ||
        query.where->kind != Predicate::Kind::all || query.where->children.empty())
        return {};
    const auto& first = query.where->children.front();
    const auto& member = first.left;
    if (first.kind != Predicate::Kind::comparison || first.operation != Compare::not_equal ||
        first.right.kind != Expr::Kind::literal || first.right.value != Value(std::int64_t{0}) ||
        member.kind != Expr::Kind::membership || !member.subquery || !member.subquery->repeatable ||
        !member.parameters.empty() || member.arguments.size() != 1 ||
        member.arguments[0].kind != Expr::Kind::column)
        return {};
    std::map<std::string, std::string> sources{{query.alias, query.table}};
    for (const auto& join : query.joins) {
        if (!join.cross || join.kind != JoinKind::inner || join.on || join.right_where)
            return {};
        sources.emplace(join.alias, join.table);
    }
    auto integer_column = [&](const Expr& e) {
        if (e.kind != Expr::Kind::column || !sources.contains(e.qualifier))
            return false;
        const auto& columns = require_table(tables, sources.at(e.qualifier))->columns;
        return std::any_of(columns.begin(), columns.end(),
                           [&](const Column& c) { return c.name == e.name && c.type == integer(); });
    };
    if (!integer_column(member.arguments[0]))
        return {};
    for (std::size_t i = 1; i < query.where->children.size(); ++i) {
        const auto& p = query.where->children[i];
        if (p.kind != Predicate::Kind::comparison || p.operation != Compare::equal ||
            !integer_column(p.left) || !integer_column(p.right))
            return {};
    }
    // A first predicate is not reached when any Cartesian input is empty.
    // Binding/shape validation has already run, but evaluation must stay lazy.
    for (const auto& [alias, table] : sources)
        if (!require_table(tables, table)->row_count)
            return {};
#ifdef CORESQL_TESTING
    if (auto* counters = detail::active_query_counters)
        ++counters->subquery_executions;
#endif
    auto data = run(tables, *member.subquery, registry);
    std::vector<Expr> candidates;
    for (auto& row : data.rows)
        candidates.push_back(literal(std::move(row[0])));
    auto needle = member.arguments[0];
    Query next = query;
    next.alias = needle.qualifier;
    next.table = sources.at(next.alias);
    needle.qualifier.clear();
    next.source_where = Predicate{membership(std::move(needle), std::move(candidates), member.name),
                                  Compare::not_equal, literal(std::int64_t{0})};
    next.joins.clear();
    std::set<std::string> joined{next.alias};
    while (joined.size() < sources.size()) {
        bool found = false;
        for (std::size_t i = 1; i < query.where->children.size(); ++i) {
            const auto& p = query.where->children[i];
            auto a = p.left.qualifier, b = p.right.qualifier;
            if (joined.contains(a) == joined.contains(b))
                continue;
            auto alias = joined.contains(a) ? b : a;
            next.joins.push_back({sources.at(alias), alias, p.left, p.right});
            joined.insert(alias);
            found = true;
            break;
        }
        if (!found) {
            for (const auto& [alias, table] : sources)
                if (!joined.contains(alias)) {
                    next.joins.push_back(
                        {table, alias, literal(std::int64_t{0}), literal(std::int64_t{0}), true});
                    joined.insert(alias);
                    break;
                }
        }
    }
    auto rest = query.where->children;
    rest.erase(rest.begin());
    next.where = all_of(std::move(rest));
    return next;
}
} // namespace coresql::detail::execution
