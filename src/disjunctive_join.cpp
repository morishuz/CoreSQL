#include "scan.hpp"

namespace coresql::detail::execution {
std::optional<Query> prepare_disjunctive_join(const Tables& tables, const Query& query,
                                              const Registry& registry) {
    if (!query.limit || query.join || query.joins.size() != 1 || query.source_where || !query.where ||
        query.where->kind != Predicate::Kind::any || query.where->children.size() < 2)
        return {};
    const auto& join = query.joins.front();
    if (!join.cross || join.kind != JoinKind::inner || join.on || join.right_where)
        return {};
    auto leading = [](const Predicate& branch) {
        const auto* p = &branch;
        while (p->kind == Predicate::Kind::all && !p->children.empty())
            p = &p->children.front();
        return p;
    };
    const auto* key = leading(query.where->children.front());
    auto equality = [](const Predicate& p) {
        return p.kind == Predicate::Kind::comparison && p.operation == Compare::equal &&
               p.left.kind == Expr::Kind::column && p.right.kind == Expr::Kind::column;
    };
    if (!equality(*key) || key->left.qualifier == key->right.qualifier)
        return {};
    auto same = [](const Expr& a, const Expr& b) { return a.qualifier == b.qualifier && a.name == b.name; };
    for (const auto& branch : query.where->children) {
        const auto* p = leading(branch);
        if (!equality(*p) || !((same(p->left, key->left) && same(p->right, key->right)) ||
                               (same(p->left, key->right) && same(p->right, key->left))))
            return {};
    }
    // A FALSE leading key suppresses every branch. UNKNOWN does not: later
    // expressions may still run or fail. Retain the original path if either
    // snapshot contains a NULL key, even when the query is marked repeatable.
    auto nonnull_native_i64 = [&](const Expr& e) {
        std::string name;
        if (e.qualifier == query.alias)
            name = query.table;
        else if (e.qualifier == join.alias)
            name = join.table;
        else
            return false;
        const auto& table = *require_table(tables, name);
        auto c = std::find_if(table.columns.begin(), table.columns.end(),
                              [&](const Column& column) { return column.name == e.name; });
        if (c == table.columns.end() || !native_i64(registry.addon(c->type)))
            return false;
        const auto index = static_cast<std::size_t>(c - table.columns.begin());
        return visit_table_rows(table, [&](auto, const Row& row, auto) {
#ifdef CORESQL_TESTING
            if (auto* counters = detail::active_query_counters)
                ++counters->rows_tested;
#endif
            return !is_null(row[index]);
        });
    };
    if (!nonnull_native_i64(key->left) || !nonnull_native_i64(key->right))
        return {};
    Query next = query;
    next.joins.front().cross = false;
    next.joins.front().left = key->left;
    next.joins.front().right = key->right;
    // Preserve FROM order, duplicate matches and the entire original WHERE.
    return next;
}
} // namespace coresql::detail::execution
