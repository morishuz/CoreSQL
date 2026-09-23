#include "bound.hpp"
#include "query_stages.hpp"
#include "scan.hpp"
#include "index.hpp"
#include "native_join.hpp"
#include "projection_cache.hpp"
#include "predicate_reuse.hpp"
#include "ordered_scan.hpp"
#include "index_row.hpp"

namespace coresql::detail::execution {
Result run_scan(const Tables& tables, const Query& query, const Registry& registry,
                const std::optional<Predicate>& early, const RowConsumer* consumer,
                const std::vector<RowView>* input_rows, const RowVisitor* visitor) {
    const auto& table = *require_table(tables, query.table);
    Scope scope(table);
    scope.left_alias = query.alias;
    scope.allow_identity = !input_rows;
    std::optional<BoundExpr> join_left, join_right;
    std::optional<detail::Comparison> join_comparison;
    bool indexed_join = false;
    NativeJoinLookup hash_join;
    const detail::OrderedIndex* ordered_join = nullptr;
    if (query.join) {
        require_distinct_join_aliases(query.alias, query.join->alias);
        scope.right = require_table(tables, query.join->table).get();
        scope.right_alias = query.join->alias;
        if (!input_rows && !query.join->cross) {
            if (query.join->left.kind != Expr::Kind::column || query.join->right.kind != Expr::Kind::column)
                fail(ErrorCode::schema, "Join operands must be columns");
            join_left = bind(query.join->left, scope, registry);
            join_right = bind(query.join->right, scope, registry);
            if (join_left->index >= table.columns.size())
                std::swap(join_left, join_right);
            if (join_left->index >= table.columns.size() || join_right->index < table.columns.size())
                fail(ErrorCode::schema, "Join must compare columns from different sides");
            if (join_left->type != join_right->type)
                fail(ErrorCode::type, "Join operand types differ");
            join_comparison.emplace(registry, join_left->type, true);
            indexed_join = scope.right->primary &&
                           join_right->index - table.columns.size() == scope.right->primary->column;
            for (const auto& index : scope.right->ordered)
                if (index->columns.front() == join_right->index - table.columns.size()) {
                    ordered_join = index.get();
                    break;
                }
        }
    }
    Result result;
    std::vector<BoundExpr> projection;
    if (query.select.empty()) {
        for (const auto& c : table.columns)
            projection.push_back(bind(column(query.alias, c.name), scope, registry));
        if (scope.right)
            for (const auto& c : scope.right->columns)
                projection.push_back(bind(column(scope.right_alias, c.name), scope, registry));
    } else {
        for (const auto& expression : query.select)
            projection.push_back(bind(expression, scope, registry));
    }
    for (const auto& expression : projection)
        result.types.push_back(expression.type);

    ProjectionCache projection_cache(projection, query.repeatable);
    auto predicate = bind_predicate(query.where, scope, registry);
    if (!input_rows && predicate && query.repeatable && query.join && query.join->cross)
        reuse_left_predicates(*predicate, table.columns.size());
    struct BoundOrder {
        BoundExpr expression;
        detail::Comparison comparison;
        bool descending, borrowed;
    };
    std::vector<BoundOrder> order;
    for (const auto& key : query.order_by) {
        auto expression = bind(key.expression, scope, registry);
        auto comparison = detail::Comparison(registry, expression.type, false);
        auto layout = registry.addon(expression.type).layout;
        bool borrowed = (expression.kind == Expr::Kind::column || expression.kind == Expr::Kind::literal) &&
                        (layout == Layout::text || layout == Layout::bytes);
        order.push_back({std::move(expression), std::move(comparison), key.descending, borrowed});
    }
    const detail::IndexBinding* search_index = nullptr;
    std::optional<IndexResult> selected;
    if (query.search) {
        if (scope.right)
            fail(ErrorCode::unsupported, "Index search cannot be combined with a join yet");
        auto expression = bind(column(query.search->column), table, registry);
        for (const auto& index : table.indexes)
            if (index->column == expression.index)
                search_index = index.get();
        if (!search_index)
            fail(ErrorCode::schema, "Column has no search index");
        if (is_null(query.search->value))
            fail(ErrorCode::type, "Index search requires a non-null value");
        registry.validate(query.search->value,
                          search_index->data->search_type(query.search->operation, expression.type));
        if (query.limit) {
            selected = search_index->data->search(query.search->operation, query.search->value);
        }
    }
    // Validate the whole query even with no rows or LIMIT 0.
    if (query.limit == 0 || (scope.right && scope.right->row_count == 0))
        return result;
    if (!scope.right && !query.search && !input_rows && !consumer && !visitor && !early)
        if (auto plan = ordered_scan_plan(table, query, predicate, registry, projection)) {
            OrderedScan scan(table, *plan);
            QueryBuffer output_memory;
            while (auto entry = scan.next()) {
                QueryBuffer traversal_memory;
                traversal_memory.add(scan.buffer_bytes());
                IndexRow row(table, *entry, plan->row_columns);
#ifdef CORESQL_TESTING
                if (auto* counters = detail::active_query_counters)
                    ++counters->rows_tested;
#endif
                if (predicate && !predicate->matches(row, registry))
                    continue;
                projection_cache.reset();
                Row output;
                output.reserve(projection.size());
                for (const auto& expression : projection)
                    output.push_back(expression.evaluate(row, registry));
                output_memory.add_row(output, sizeof(Row));
                result.rows.push_back(std::move(output));
                if (result.rows.size() == query.limit)
                    break;
            }
#ifdef CORESQL_TESTING
            detail::record_query_stage("ordered_scan", result);
#endif
            return result;
        }
    if (!scope.right && !query.search)
        selected = candidates(table, predicate, registry);
    QueryBuffer candidate_memory;
    if (selected) {
        candidate_memory.add(selected->rows.size() * sizeof(RowLocation));
        normalize(*selected);
    }
    // Keep the first owned key inline: single-key sorts need no per-row allocation.
    // Text/custom column and literal keys borrow from the immutable query snapshot.
    struct Match {
        RowView row;
        Value key;
        std::vector<Value> more_keys;
        std::size_t ordinal;
        std::shared_ptr<Chunk> left_pin, right_pin;
    };
    std::vector<Match> matches;
    QueryBuffer match_memory, result_memory;
    auto match_bytes = [&](const Match& match) {
        if (!match_memory.active())
            return std::size_t{0};
        std::size_t bytes = 2 * sizeof(Match) + match.more_keys.size() * sizeof(Value);
        bytes += value_buffer_bytes(match.key);
        for (const auto& value : match.more_keys)
            bytes += value_buffer_bytes(value);
        return bytes;
    };
    const bool bounded =
        !order.empty() && (scope.right ? query.limit != std::numeric_limits<std::size_t>::max()
                                       : query.limit < table.row_count);
    auto key = [&](const Match& match, std::size_t i) -> const Value& {
        const auto& o = order[i];
        if (!o.borrowed)
            return i == 0 ? match.key : match.more_keys[i - 1];
        return o.expression.kind == Expr::Kind::column ? match.row[o.expression.index] : o.expression.value;
    };
    auto before = [&](const Match& a, const Match& b) {
        query_step();
        for (std::size_t i = 0; i < order.size(); ++i) {
            int c = order[i].comparison.compare(key(a, i), key(b, i));
            if (c)
                return order[i].descending ? c > 0 : c < 0;
        }
        // Scan ordinals preserve ties for both full sorts and bounded heaps.
        return a.ordinal < b.ordinal;
    };
    auto prefix = bind_predicate(early, scope, registry);
    const auto* direct = predicate ? predicate->direct_comparison() : nullptr;
    std::size_t ordinal = 0;
    std::exception_ptr projection_error;
    Row projected;
    if (consumer)
        projected.reserve(projection.size());
    std::shared_ptr<Chunk> left_pin, right_pin;
    auto accept = [&](const auto& row) {
        query_step();
#ifdef CORESQL_TESTING
        if (auto* counters = detail::active_query_counters)
            ++counters->rows_tested;
#endif
        // UNKNOWN must survive: a later WHERE expression can still run or fail.
        if (prefix && prefix->truth(row, registry) == 0)
            return true;
        if (search_index && !selected->exact &&
            !search_index->data->matches(query.search->operation, row[search_index->column],
                                         query.search->value))
            return true;
        if (predicate && (direct ? !direct->values(row[direct->left.index], direct->right.value)
                                 : !predicate->matches(row, registry)))
            return true;
        if (visitor) {
            projection_cache.reset();
            projected.clear();
            for (const auto& expression : projection)
                projected.push_back(expression.evaluate(row, registry));
            QueryBuffer projection_memory;
            projection_memory.add_row(projected);
            return (*visitor)(projected) && ++ordinal != query.limit;
        }
        if (consumer) {
            // Repeatable aggregate inputs need not retain rows. Continue WHERE
            // after a projection failure: the original scan evaluated all WHERE
            // expressions first, and their errors must still win.
            if (!projection_error) {
                try {
                    projection_cache.reset();
                    projected.clear();
                    for (const auto& expression : projection)
                        projected.push_back(expression.evaluate(row, registry));
                    QueryBuffer projection_memory;
                    projection_memory.add_row(projected);
                    (*consumer)(projected);
                } catch (...) {
                    projection_error = std::current_exception();
                }
            }
            return ++ordinal != query.limit;
        }
        // Evaluate every matching key, including rows outside the final top k.
        Match match{row, std::int64_t{0}, {}, ordinal++, left_pin, right_pin};
        if (order.size() > 1)
            match.more_keys.resize(order.size() - 1, std::int64_t{0});
        for (std::size_t i = 0; i < order.size(); ++i)
            if (!order[i].borrowed) {
                auto value = order[i].expression.evaluate(row, registry);
                if (i == 0)
                    match.key = std::move(value);
                else
                    match.more_keys[i - 1] = std::move(value);
            }
        if (bounded && matches.size() == query.limit) {
            if (before(match, matches.front())) {
                std::pop_heap(matches.begin(), matches.end(), before);
                match_memory.remove(match_bytes(matches.back()));
                match_memory.add(match_bytes(match));
                matches.back() = std::move(match);
                std::push_heap(matches.begin(), matches.end(), before);
            }
        } else {
            match_memory.add(match_bytes(match));
            matches.push_back(std::move(match));
            if (bounded && matches.size() == query.limit)
                std::make_heap(matches.begin(), matches.end(), before);
        }
#ifdef CORESQL_TESTING
        detail::retained_matches() = std::max(detail::retained_matches(), matches.size());
#endif
        return !order.empty() || matches.size() != query.limit;
    };
    // Keep the common scan loop direct; row-selection and join machinery is cold.
    if (input_rows) {
        for (const auto& row : *input_rows)
            if (!accept(row))
                break;
    } else if (!selected && !scope.right && !table.selection) {
        bool more = true;
        for (const auto& [id, reference] : table.chunks) {
            const auto chunk = reference.pin();
            left_pin = reference.paged() ? chunk : nullptr;
            (void)id;
#ifdef CORESQL_TESTING
            ++detail::visited_chunks();
#endif
            for (std::size_t i = 0; i < chunk->rows.size(); ++i)
                if (!accept(RowView(chunk->rows[i], chunk->rowids[i]))) {
                    more = false;
                    break;
                }
            if (!more)
                break;
        }
    } else {
        // WHERE stays after ON for joins; pushing it down could change error behavior.
        visit_chunks(table, selected, [&](auto id, const auto& chunk, RowSelection rows) {
            left_pin = table.chunks.find(id)->second.paged() ? chunk : nullptr;
            return visit_rows(*chunk, rows, [&](auto position, const Row& row) {
                if (!scope.right)
                    return accept(RowView(row, chunk->rowids[position]));
                const auto& value = query.join->cross ? Value(std::int64_t{0}) : row[join_left->index];
                if (!query.join->cross && is_null(value))
                    return true;
                if (ordered_join) {
                    auto locations = ordered_join->range(value, value);
                    std::sort(locations.begin(), locations.end());
                    for (auto location : locations) {
#ifdef CORESQL_TESTING
                        if (auto* counters = detail::active_query_counters)
                            ++counters->candidate_pairs;
#endif
                        const auto other = indexed_row(*scope.right, location);
                        right_pin = other.chunk;
                        if (!accept(RowView(row, other, chunk->rowids[position])))
                            return false;
                    }
                    return true;
                }
                if (indexed_join) {
                    auto location = detail::lookup(*scope.right, value);
                    if (!location)
                        return true;
#ifdef CORESQL_TESTING
                    ++detail::visited_chunks();
                    if (auto* counters = detail::active_query_counters)
                        ++counters->candidate_pairs;
#endif
                    const auto other = indexed_row(*scope.right, *location);
                    right_pin = other.chunk;
                    return accept(RowView(row, other, chunk->rowids[position]));
                }
                if (!query.join->cross && native_join_key(registry.addon(join_left->type))) {
                    const auto& candidates =
                        hash_join.find(*scope.right, join_right->index - table.columns.size(), value,
                                       registry, join_left->type);
                    for (const auto* other : candidates) {
#ifdef CORESQL_TESTING
                        if (auto* counters = detail::active_query_counters)
                            ++counters->candidate_pairs;
#endif
                        if (!accept(RowView(row, *other, chunk->rowids[position])))
                            return false;
                    }
                    return true;
                }
                return visit_table_rows(*scope.right, [&](auto, const Row& other, auto, const auto& pin) {
                    right_pin = pin;
#ifdef CORESQL_TESTING
                    if (auto* counters = detail::active_query_counters)
                        ++counters->candidate_pairs;
#endif
                    if (!query.join->cross &&
                        !join_comparison->equal(value, other[join_right->index - table.columns.size()]))
                        return true;
                    return accept(RowView(row, other, chunk->rowids[position]));
                });
            });
        });
    }
    if (consumer || visitor) {
        if (projection_error)
            std::rethrow_exception(projection_error);
        return result;
    }
    if (!order.empty()) {
        if (bounded && matches.size() == query.limit)
            std::sort_heap(matches.begin(), matches.end(), before);
        else
            std::stable_sort(matches.begin(), matches.end(), before);
    }
    const auto count = matches.size();
    result_memory.add(count * sizeof(Row));
    result.rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Row row;
        row.reserve(projection.size());
        projection_cache.reset();
        auto project = [&](const auto& input) {
            for (const auto& expression : projection)
                row.push_back(expression.evaluate(input, registry));
        };
        project(matches[i].row);
        result_memory.add_row(row);
        result.rows.push_back(std::move(row));
    }
#ifdef CORESQL_TESTING
    detail::record_query_stage("scan", result, matches.capacity() * sizeof(Match));
#endif
    return result;
}

} // namespace coresql::detail::execution
