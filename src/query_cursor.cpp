#include "bound.hpp"
#include "index.hpp"
#include "ordered_index.hpp"
#include "ordered_scan.hpp"
#include "index_row.hpp"
#include "scan.hpp"
#include <array>
#include <coroutine>
#include <utility>

namespace coresql::detail::execution {
namespace {
// The suspended frame retains only its current input pins and bound expressions.
// It never starts a producer thread or queues undispatched output rows.
template <class T> class Generator {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;
        Generator get_return_object() {
            return Generator(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T next) {
            value.emplace(std::move(next));
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };
    Generator() = default;
    explicit Generator(std::coroutine_handle<promise_type> frame) : frame_(frame) {}
    Generator(Generator&& other) noexcept : frame_(std::exchange(other.frame_, {})) {}
    Generator& operator=(Generator&& other) noexcept {
        if (this != &other) {
            if (frame_)
                frame_.destroy();
            frame_ = std::exchange(other.frame_, {});
        }
        return *this;
    }
    ~Generator() {
        if (frame_)
            frame_.destroy();
    }
    bool advance() {
        if (!frame_ || frame_.done())
            return false;
        frame_.promise().value.reset();
        frame_.resume();
        if (frame_.promise().error)
            std::rethrow_exception(frame_.promise().error);
        return !frame_.done();
    }
    T& value() { return *frame_.promise().value; }

private:
    std::coroutine_handle<promise_type> frame_;
};

template <class Ref> auto pin_chunk(const Ref& ref) {
    if constexpr (requires { ref.pin(); })
        return ref.pin();
    else
        return ref;
}

Generator<RowView> source_rows(const Table& table, const Value* primary_key = nullptr) {
    if (primary_key) {
        const auto location = lookup(table, *primary_key);
        if (!location || (table.selection &&
                          !std::binary_search(table.selection->begin(), table.selection->end(), *location)))
            co_return;
        query_step();
        auto found = table.chunks.find(location->chunk);
        if (found == table.chunks.end())
            fail(ErrorCode::state, "Cursor primary index references a missing chunk");
        auto pinned = pin_chunk(found->second);
        auto position = pinned->position(location->slot);
        if (position == pinned->rows.size())
            fail(ErrorCode::state, "Cursor primary index references a missing row");
#ifdef CORESQL_TESTING
        ++detail::visited_chunks();
#endif
        co_yield RowView(pinned->rows[position], pinned->rowids[position]);
        co_return;
    }
    if (table.selection) {
        for (auto location : *table.selection) {
            query_step();
            auto found = table.chunks.find(location.chunk);
            if (found == table.chunks.end())
                fail(ErrorCode::state, "Cursor selection references a missing chunk");
            auto pinned = pin_chunk(found->second);
            auto position = pinned->position(location.slot);
            if (position == pinned->rows.size())
                fail(ErrorCode::state, "Cursor selection references a missing row");
            co_yield RowView(pinned->rows[position], pinned->rowids[position]);
        }
        co_return;
    }
    for (const auto& [id, ref] : table.chunks) {
        (void)id;
        auto pinned = pin_chunk(ref);
#ifdef CORESQL_TESTING
        ++detail::visited_chunks();
#endif
        for (std::size_t i = 0; i < pinned->rows.size(); ++i) {
            query_step();
            co_yield RowView(pinned->rows[i], pinned->rowids[i]);
        }
    }
}

void validate_streamable(const Query& query, unsigned depth = 0) {
    if (depth >= 64)
        fail(ErrorCode::schema, "Cursor query nesting exceeds 64");
    const bool joined = query.join || !query.joins.empty();
    if (!query.group_by.empty() || query.having || query.distinct || !query.relations.empty() ||
        query.search || query.joins.size() > 2 || (query.join && !query.joins.empty()) ||
        std::any_of(query.select.begin(), query.select.end(), has_aggregate))
        fail(ErrorCode::unsupported,
             "Cursor requires scans, an ordered index scan, one or two INNER/LEFT joins, or UNION ALL");
    if (!query.order_by.empty() && (joined || !query.compounds.empty()))
        fail(ErrorCode::unsupported, "Cursor ORDER BY supports a single table");
    auto reject_join = [&](const Join& join) {
        if (join.kind != JoinKind::inner && join.kind != JoinKind::left)
            fail(ErrorCode::unsupported, "Cursor supports INNER and LEFT joins");
        if (join.right_where && !query.repeatable)
            fail(ErrorCode::unsupported, "Cursor right-side filtering requires repeatable expressions");
    };
    if (query.join)
        reject_join(*query.join);
    for (const auto& join : query.joins)
        reject_join(join);
    for (const auto& [op, arm] : query.compounds) {
        if (op != SetOperation::union_all)
            fail(ErrorCode::unsupported, "Cursor supports UNION ALL only");
        if (!arm)
            fail(ErrorCode::schema, "Missing compound query");
        validate_streamable(*arm, depth + 1);
    }
}

// One bound shape shared by cursor open and resumption. Compound arms are bound
// with the parent, so opening a cursor does not plan the query a second time.
struct StreamPlan {
    const Table* left = nullptr;
    const Table* inputs[2] = {};
    const Join* joins[2] = {};
    int join_count = 0;
    std::optional<BoundPredicate> where, source_where;
    std::optional<BoundPredicate> on[2], side_where[2];
    std::optional<std::size_t> key_column[2];
    std::vector<BoundExpr> projection;
    std::vector<Type> types;
    const OrderedIndex* order = nullptr;
    bool reverse_order = false;
    std::optional<OrderedScanPlan> ordered;
    std::vector<StreamPlan> arms;
};
struct Parts {
    const Row* left = nullptr;
    std::int64_t id = 0;
    const Row* row[3] = {};
    std::size_t width[3] = {};
    int count = 0;
    const Value& operator[](std::size_t index) const {
        for (int part = 0; part < count; ++part) {
            if (index < width[part])
                return (*row[part])[index];
            index -= width[part];
        }
        fail(ErrorCode::state, "Column index is outside the joined row");
    }
};
Parts part(const Row& row, std::int64_t id = 0) {
    Parts parts;
    parts.left = &row;
    parts.id = id;
    parts.row[0] = &row;
    parts.width[0] = row.size();
    parts.count = 1;
    return parts;
}
Parts joined(const Parts& prefix, const Row& row) {
    Parts parts = prefix;
    parts.row[parts.count] = &row;
    parts.width[parts.count] = row.size();
    ++parts.count;
    return parts;
}
Row nulls_for(const Table& table) {
    Row row;
    for (const auto& column : table.columns)
        row.push_back(Null(column.type));
    return row;
}
const OrderedIndex* matching_order(const Table& table, const Query& query, bool& reverse) {
    for (const auto& index : table.ordered) {
        if (query.order_by.size() > index->columns.size())
            continue;
        std::optional<bool> flip;
        bool matched = true;
        for (std::size_t i = 0; i < query.order_by.size(); ++i) {
            const auto& expression = query.order_by[i].expression;
            if (expression.kind != Expr::Kind::column ||
                (!expression.qualifier.empty() && expression.qualifier != query.alias)) {
                matched = false;
                break;
            }
            auto found = std::find_if(table.columns.begin(), table.columns.end(),
                                      [&](const Column& column) { return column.name == expression.name; });
            if (found == table.columns.end() ||
                static_cast<std::size_t>(found - table.columns.begin()) != index->columns[i]) {
                matched = false;
                break;
            }
            const bool descending = !index->definition.descending.empty() && index->definition.descending[i];
            const bool key_flip = query.order_by[i].descending != descending;
            if (!flip)
                flip = key_flip;
            else if (*flip != key_flip) {
                matched = false;
                break;
            }
        }
        if (!matched)
            continue;
        reverse = flip.value_or(false);
        return index.get();
    }
    fail(ErrorCode::unsupported, "Cursor ORDER BY requires a matching ordered index");
}
void check_equality_sides(const Join& join, const Scope& scope, const Registry& registry, std::size_t offset,
                          std::size_t width) {
    if (join.cross || join.on)
        return;
    if (join.left.kind != Expr::Kind::column || join.right.kind != Expr::Kind::column)
        fail(ErrorCode::schema, "Join operands must be columns");
    auto left = bind(join.left, scope, registry);
    auto right = bind(join.right, scope, registry);
    if (left.index >= offset)
        std::swap(left, right);
    if (left.index >= offset || right.index < offset || right.index >= offset + width)
        fail(ErrorCode::schema, "Join must compare columns from different sides");
    if (left.type != right.type)
        fail(ErrorCode::type, "Join operand types differ");
}
std::optional<std::size_t> primary_join_key(const BoundPredicate& on, const Table& right,
                                            std::size_t right_start) {
    if (!right.primary || !on.leaf || on.leaf->operation != Compare::equal)
        return {};
    const auto right_key = right_start + right.primary->column;
    const auto& left = on.leaf->left;
    const auto& other = on.leaf->right;
    if (left.kind == Expr::Kind::column && other.kind == Expr::Kind::column && other.index == right_key &&
        left.index < right_start)
        return left.index;
    if (other.kind == Expr::Kind::column && left.kind == Expr::Kind::column && left.index == right_key &&
        other.index < right_start)
        return other.index;
    return {};
}
StreamPlan bind_stream(const Tables& tables, const Query& query, const Registry& registry) {
    StreamPlan plan;
    Table scalar;
    plan.left = query.table.empty() ? nullptr : require_table(tables, query.table).get();
    const auto& left = plan.left ? *plan.left : scalar;
    if (query.join)
        plan.joins[plan.join_count++] = &*query.join;
    for (const auto& join : query.joins)
        plan.joins[plan.join_count++] = &join;
    if (plan.join_count == 0 && query.source_where)
        fail(ErrorCode::schema, "source_where requires joins");
    for (int i = 0; i < plan.join_count; ++i) {
        require_distinct_join_aliases(query.alias, plan.joins[i]->alias);
        for (int earlier = 0; earlier < i; ++earlier)
            require_distinct_join_aliases(plan.joins[earlier]->alias, plan.joins[i]->alias);
        plan.inputs[i] = require_table(tables, plan.joins[i]->table).get();
    }
    Scope scope(left);
    scope.left_alias = query.alias;
    scope.allow_identity = plan.join_count == 0;
    std::size_t offset = left.columns.size();
    for (int i = 0; i < plan.join_count; ++i) {
        // Each ON sees only tables joined so far. A later alias is not in scope yet.
        if (i == 0) {
            scope.right = plan.inputs[0];
            scope.right_alias = plan.joins[0]->alias;
        } else {
            scope.third = plan.inputs[1];
            scope.third_alias = plan.joins[1]->alias;
        }
        const auto& join = *plan.joins[i];
        check_equality_sides(join, scope, registry, offset, plan.inputs[i]->columns.size());
        auto condition = join.on;
        if (!condition && !join.cross)
            condition = Predicate{join.left, Compare::equal, join.right};
        plan.on[i] = bind_predicate(condition, scope, registry);
        Scope side(*plan.inputs[i]);
        side.left_alias = join.alias;
        plan.side_where[i] = bind_predicate(join.right_where, side, registry);
        if (plan.on[i] && !plan.side_where[i])
            plan.key_column[i] = primary_join_key(*plan.on[i], *plan.inputs[i], offset);
        offset += plan.inputs[i]->columns.size();
    }
    plan.where = bind_predicate(query.where, scope, registry);
    Scope left_scope(left);
    left_scope.left_alias = query.alias;
    plan.source_where = bind_predicate(query.source_where, left_scope, registry);
    if (query.select.empty()) {
        for (const auto& column : left.columns)
            plan.projection.push_back(bind(coresql::column(query.alias, column.name), scope, registry));
        for (int i = 0; i < plan.join_count; ++i)
            for (const auto& column : plan.inputs[i]->columns)
                plan.projection.push_back(
                    bind(coresql::column(plan.joins[i]->alias, column.name), scope, registry));
    } else
        for (const auto& expression : query.select)
            plan.projection.push_back(bind(expression, scope, registry));
    for (const auto& expression : plan.projection)
        plan.types.push_back(expression.type);
    if (!query.order_by.empty()) {
        plan.ordered = ordered_scan_plan(left, query, plan.where, registry, plan.projection);
        if (!plan.ordered)
            plan.order = matching_order(left, query, plan.reverse_order);
    }
    return plan;
}
StreamPlan bind_tree(const Tables& tables, const Query& query, const Registry& registry) {
    auto plan = bind_stream(tables, query, registry);
    for (const auto& [op, arm] : query.compounds) {
        (void)op;
        auto bound = bind_tree(tables, *arm, registry);
        if (bound.types != plan.types)
            fail(ErrorCode::type, "Compound query column types differ");
        plan.arms.push_back(std::move(bound));
    }
    return plan;
}
Generator<Row> scan(const Tables&, const Query&, const Registry& registry, StreamPlan plan) {
    auto project = [&](const auto& row) -> std::optional<Row> {
        query_step();
#ifdef CORESQL_TESTING
        if (auto* counters = detail::active_query_counters)
            ++counters->rows_tested;
#endif
        if (plan.where && !plan.where->matches(row, registry))
            return {};
        Row output;
        output.reserve(plan.projection.size());
        for (const auto& expression : plan.projection)
            output.push_back(expression.evaluate(row, registry));
        return output;
    };
    if (!plan.left) {
        const Row empty;
        if (auto row = project(part(empty, 1)))
            co_yield std::move(*row);
        co_return;
    }
    const Value* primary_key = nullptr;
    if (plan.join_count == 0 && !plan.order && !plan.ordered && plan.left->primary && plan.where) {
        const BoundPredicate* guard = &*plan.where;
        while (guard->kind == Predicate::Kind::all && !guard->children.empty())
            guard = &guard->children.front();
        const auto* direct = guard->direct_comparison();
        if (direct && direct->operation == Compare::equal &&
            direct->left.index == plan.left->primary->column && !is_null(direct->right.value))
            primary_key = &direct->right.value;
    }
    if (plan.ordered) {
        OrderedScan scan(*plan.left, *plan.ordered);
        while (auto entry = scan.next()) {
            std::optional<Row> output;
            {
                // Re-account suspended storage against this call's control.
                // No QueryBuffer may survive co_yield and retain a stale control.
                QueryBuffer memory;
                memory.add(scan.buffer_bytes());
                IndexRow row(*plan.left, *entry, plan.ordered->row_columns);
                output = project(row);
                if (output)
                    memory.add_row(*output, sizeof(Row));
            }
            if (output)
                co_yield std::move(*output);
        }
        co_return;
    }
    if (plan.order) {
        OrderedIndex::Walk walk(*plan.order, plan.reverse_order);
        while (auto location = walk.next()) {
            if (plan.left->selection &&
                !std::binary_search(plan.left->selection->begin(), plan.left->selection->end(), *location))
                continue;
            query_step();
            auto found = plan.left->chunks.find(location->chunk);
            if (found == plan.left->chunks.end())
                fail(ErrorCode::state, "Cursor ordered index references a missing chunk");
            auto pinned = pin_chunk(found->second);
            auto position = pinned->position(location->slot);
            if (position == pinned->rows.size())
                fail(ErrorCode::state, "Cursor ordered index references a missing row");
            if (auto row = project(part(pinned->rows[position], pinned->rowids[position])))
                co_yield std::move(*row);
        }
        co_return;
    }
    Row null_input[2];
    for (int i = 0; i < plan.join_count; ++i)
        if (plan.joins[i]->kind == JoinKind::left)
            null_input[i] = nulls_for(*plan.inputs[i]);
    auto left_rows = source_rows(*plan.left, primary_key);
    while (left_rows.advance()) {
        auto left = left_rows.value();
        auto base = part(*left.left, left.id);
        if (plan.source_where && !plan.source_where->matches(base, registry))
            continue;
        // Own the descriptor across suspension, including NULL-extended temporaries.
        auto probe_second = [&](Parts pair) -> Generator<Row> {
            if (plan.join_count < 2) {
                if (auto row = project(pair))
                    co_yield std::move(*row);
                co_return;
            }
            bool matched_second = false;
            const Value* second_key = plan.key_column[1] ? &pair[*plan.key_column[1]] : nullptr;
            auto second_rows = source_rows(*plan.inputs[1], second_key);
            while ((!second_key || !is_null(*second_key)) && second_rows.advance()) {
                auto second = second_rows.value();
                if (plan.side_where[1] && !plan.side_where[1]->matches(part(*second.left), registry))
                    continue;
                auto triple = joined(pair, *second.left);
                query_step();
                if (plan.on[1] && !plan.on[1]->matches(triple, registry))
                    continue;
                matched_second = true;
                if (auto row = project(triple))
                    co_yield std::move(*row);
            }
            if (!matched_second && plan.joins[1]->kind == JoinKind::left)
                if (auto row = project(joined(pair, null_input[1])))
                    co_yield std::move(*row);
        };
        if (plan.join_count == 0) {
            if (auto row = project(base))
                co_yield std::move(*row);
            continue;
        }
        bool matched_first = false;
        const Value* first_key = plan.key_column[0] ? &(*left.left)[*plan.key_column[0]] : nullptr;
        auto first_rows = source_rows(*plan.inputs[0], first_key);
        while ((!first_key || !is_null(*first_key)) && first_rows.advance()) {
            auto first = first_rows.value();
            if (plan.side_where[0] && !plan.side_where[0]->matches(part(*first.left), registry))
                continue;
            auto pair = joined(base, *first.left);
            query_step();
            if (plan.on[0] && !plan.on[0]->matches(pair, registry))
                continue;
            matched_first = true;
            auto matches = probe_second(pair);
            while (matches.advance())
                co_yield std::move(matches.value());
        }
        if (!matched_first && plan.joins[0]->kind == JoinKind::left) {
            auto matches = probe_second(joined(base, null_input[0]));
            while (matches.advance())
                co_yield std::move(matches.value());
        }
    }
}

Generator<Row> rows(const Tables& tables, Query query, const Registry& registry, StreamPlan plan) {
    if (!query.limit)
        co_return;
    const auto limit = query.limit;
    auto remaining_offset = query.offset;
    std::size_t emitted = 0;
    auto compounds = std::move(query.compounds);
    auto arms = std::move(plan.arms);
    query.compounds.clear();
    query.offset = 0;
    query.limit = std::numeric_limits<std::size_t>::max();
    auto input = scan(tables, query, registry, std::move(plan));
    while (input.advance()) {
        if (remaining_offset) {
            --remaining_offset;
            continue;
        }
        co_yield std::move(input.value());
        if (++emitted == limit)
            co_return;
    }
    for (std::size_t i = 0; i < compounds.size(); ++i) {
        auto input = rows(tables, *compounds[i].second, registry, std::move(arms[i]));
        while (input.advance()) {
            if (remaining_offset) {
                --remaining_offset;
                continue;
            }
            co_yield std::move(input.value());
            if (++emitted == limit)
                co_return;
        }
    }
}

struct BindingScope {
    const Tables* previous;
    explicit BindingScope(const Tables& tables) : previous(binding_tables) { binding_tables = &tables; }
    ~BindingScope() { binding_tables = previous; }
};
} // namespace
} // namespace coresql::detail::execution

namespace coresql {
struct QueryCursor::Impl {
    detail::Tables tables;
    Registry registry;
    Query query;
    QueryOptions options;
    std::vector<Type> types;
    detail::execution::QueryUsage usage;
    CursorStats counters;
    std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
    detail::execution::Generator<Row> input;
    Impl(detail::Tables t, Registry r, Query q, QueryOptions o)
        : tables(std::move(t)), registry(std::move(r)), query(std::move(q)), options(std::move(o)) {
        using namespace detail::execution;
        QueryControl control(options, &usage);
        BindingScope binding(tables);
        validate_streamable(query);
        auto plan = bind_tree(tables, query, registry);
        types = plan.types;
        input = rows(tables, query, registry, std::move(plan));
        for (const auto& [name, table] : tables) {
            (void)name;
            counters.snapshot_payload_bytes += table->payload_bytes;
        }
        control.check();
    }
    void release() noexcept {
        input = {};
        tables.clear();
        counters.snapshot_payload_bytes = 0;
        counters.closed = true;
    }
};
QueryCursor::QueryCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
QueryCursor::QueryCursor(QueryCursor&&) noexcept = default;
QueryCursor& QueryCursor::operator=(QueryCursor&&) noexcept = default;
QueryCursor::~QueryCursor() = default;
const std::vector<Type>& QueryCursor::types() const {
    if (!impl_)
        throw Error(ErrorCode::state, "Cursor was moved from");
    return impl_->types;
}
std::optional<Row> QueryCursor::next() {
    if (!impl_)
        throw Error(ErrorCode::state, "Cursor was moved from");
    if (impl_->counters.closed)
        return {};
    try {
        using namespace detail::execution;
        QueryControl control(impl_->options, &impl_->usage);
        BindingScope binding(impl_->tables);
        if (!impl_->input.advance()) {
            impl_->release();
            return {};
        }
        QueryBuffer output;
        output.add_row(impl_->input.value());
        control.check();
        ++impl_->counters.rows;
        return std::move(impl_->input.value());
    } catch (...) {
        impl_->release();
        throw;
    }
}
Result QueryCursor::fetch(std::size_t max_rows) {
    Result result{types(), {}};
    if (!max_rows || impl_->counters.closed)
        return result;
    using namespace detail::execution;
    try {
        QueryControl control(impl_->options, &impl_->usage);
        QueryBuffer buffer;
        while (result.rows.size() < max_rows) {
            auto row = next();
            if (!row)
                break;
            // next() releases its one-row accounting before ownership enters this batch.
            buffer.add_row(*row, sizeof(Row));
            result.rows.push_back(std::move(*row));
        }
        return result;
    } catch (...) {
        close();
        throw;
    }
}
CursorStats QueryCursor::stats() const {
    if (!impl_)
        throw Error(ErrorCode::state, "Cursor was moved from");
    auto result = impl_->counters;
    result.work = impl_->usage.work;
    result.peak_buffer_bytes = impl_->usage.peak_buffer;
    result.age = std::chrono::steady_clock::now() - impl_->opened;
    return result;
}
void QueryCursor::close() noexcept {
    if (impl_)
        impl_->release();
}
} // namespace coresql

namespace coresql::detail::execution {
QueryCursor make_cursor(Tables tables, Registry registry, Query query, QueryOptions options) {
    return QueryCursor(std::make_unique<QueryCursor::Impl>(std::move(tables), std::move(registry),
                                                           std::move(query), std::move(options)));
}
} // namespace coresql::detail::execution
