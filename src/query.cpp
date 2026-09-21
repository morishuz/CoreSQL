#include "bound.hpp"
#include "query_stages.hpp"

namespace coresql::detail::execution {
namespace {
thread_local unsigned query_depth = 0;
} // namespace

Result run(const detail::Tables& tables, const Query& query, const Registry& registry) {
    query_step();
    struct BindingContext {
        const detail::Tables* before = binding_tables;
        explicit BindingContext(const detail::Tables& t) {
            if (query_depth >= 64)
                fail(ErrorCode::schema, "Query nesting exceeds 64");
            ++query_depth;
            binding_tables = &t;
        }
        ~BindingContext() {
            binding_tables = before;
            --query_depth;
        }
    } context(tables);
    if (query.offset) {
        Query next = query;
        next.offset = 0;
        if (next.limit && next.limit != std::numeric_limits<std::size_t>::max()) {
            if (query.offset > std::numeric_limits<std::size_t>::max() - next.limit)
                fail(ErrorCode::schema, "LIMIT plus OFFSET overflow");
            next.limit += query.offset;
        }
        auto result = run(tables, next, registry);
        result.rows.erase(result.rows.begin(), result.rows.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                                         query.offset, result.rows.size())));
        return result;
    }
    if (!query.relations.empty())
        return run_relations(tables, query, registry);
    if (!query.compounds.empty())
        return run_compound(tables, query, registry);
    if (query.table.empty()) {
        auto working = tables;
        Query next = query;
        next.table = materialize(working, Result{{}, {Row{}}});
        return run(working, next, registry);
    }
    if (!query.joins.empty())
        return run_multi_join(tables, query, registry);
    if (query.join && query.join->right_where)
        return run_filtered_join(tables, query, registry);
    if (query.join && (query.join->kind != JoinKind::inner || query.join->on))
        return run_general_join(tables, query, registry);
    if (query.source_where)
        fail(ErrorCode::schema, "source_where requires joins");
    if (!query.group_by.empty() || query.having ||
        std::any_of(query.select.begin(), query.select.end(), has_aggregate) ||
        std::any_of(query.order_by.begin(), query.order_by.end(),
                    [](const Order& o) { return has_aggregate(o.expression); }))
        return run_grouped(tables, query, registry);
    if (query.distinct)
        return run_distinct(tables, query, registry);
    return run_scan(tables, query, registry);
}

StreamResult stream(const Tables& tables, const Query& query, const Registry& registry,
                    const RowVisitor& visit) {
    if (!visit)
        fail(ErrorCode::state, "Streaming requires a row visitor");
    if (query.table.empty() || query.join || !query.joins.empty() || !query.relations.empty() ||
        !query.compounds.empty() || !query.order_by.empty() || !query.group_by.empty() || query.having ||
        query.distinct || query.offset || query.source_where ||
        std::any_of(query.select.begin(), query.select.end(), has_aggregate))
        fail(ErrorCode::unsupported,
             "Streaming requires a single-table scan without ordering, grouping or offset");
    // Establish normal binding/nesting context and validate the entire shape first.
    Query shape = query;
    shape.limit = 0;
    auto types = run(tables, shape, registry).types;
    if (query_depth >= 64)
        fail(ErrorCode::schema, "Query nesting exceeds 64");
    ++query_depth;
    const auto* previous = binding_tables;
    struct Restore {
        const Tables* previous;
        ~Restore() {
            binding_tables = previous;
            --query_depth;
        }
    } restore{previous};
    binding_tables = &tables;
    StreamResult result{std::move(types)};
    RowVisitor counted = [&](std::span<const Value> row) {
        ++result.rows;
        if (!visit(row)) {
            result.stopped = true;
            return false;
        }
        return true;
    };
    run_scan(tables, query, registry, {}, nullptr, nullptr, &counted);
    return result;
}

} // namespace coresql::detail::execution
