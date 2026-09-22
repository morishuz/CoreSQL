#include "bound.hpp"
#include "index.hpp"
#include "scan.hpp"
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
    if (!query.order_by.empty() || !query.group_by.empty() || query.having || query.distinct ||
        !query.relations.empty() || query.search || query.joins.size() > 1 ||
        (query.join && !query.joins.empty()) ||
        std::any_of(query.select.begin(), query.select.end(), has_aggregate))
        fail(ErrorCode::unsupported,
             "Cursor requires scans, a single INNER/LEFT join, or UNION ALL without blocking operators");
    const Join* join = query.join ? &*query.join : query.joins.empty() ? nullptr : &query.joins[0];
    if (join && join->kind != JoinKind::inner && join->kind != JoinKind::left)
        fail(ErrorCode::unsupported, "Cursor supports INNER and LEFT joins");
    if (join && join->right_where && !query.repeatable)
        fail(ErrorCode::unsupported, "Cursor right-side filtering requires repeatable expressions");
    for (const auto& [op, arm] : query.compounds) {
        if (op != SetOperation::union_all)
            fail(ErrorCode::unsupported, "Cursor supports UNION ALL only");
        if (!arm)
            fail(ErrorCode::schema, "Missing compound query");
        validate_streamable(*arm, depth + 1);
    }
}

Generator<Row> scan(const Tables& tables, const Query& query, const Registry& registry) {
    Table scalar;
    const auto& left = query.table.empty() ? scalar : *require_table(tables, query.table);
    Scope scope(left);
    scope.left_alias = query.alias;
    scope.allow_identity = !query.join && query.joins.empty();
    const Join* join = query.join ? &*query.join : query.joins.empty() ? nullptr : &query.joins[0];
    std::optional<BoundPredicate> on, right_where;
    if (join) {
        scope.right = require_table(tables, join->table).get();
        scope.right_alias = join->alias;
        auto condition = join->on;
        if (!condition && !join->cross)
            condition = Predicate{join->left, Compare::equal, join->right};
        on = bind_predicate(condition, scope, registry);
        Scope right_scope(*scope.right);
        right_scope.left_alias = join->alias;
        right_where = bind_predicate(join->right_where, right_scope, registry);
    }
    auto where = bind_predicate(query.where, scope, registry);
    Scope left_scope(left);
    left_scope.left_alias = query.alias;
    auto source_where = bind_predicate(query.source_where, left_scope, registry);
    std::vector<BoundExpr> projection;
    if (query.select.empty()) {
        for (const auto& c : left.columns)
            projection.push_back(bind(column(query.alias, c.name), scope, registry));
        if (scope.right)
            for (const auto& c : scope.right->columns)
                projection.push_back(bind(column(scope.right_alias, c.name), scope, registry));
    } else
        for (const auto& expression : query.select)
            projection.push_back(bind(expression, scope, registry));
    auto project = [&](const RowView& row) -> std::optional<Row> {
        query_step();
#ifdef CORESQL_TESTING
        if (auto* counters = detail::active_query_counters)
            ++counters->rows_tested;
#endif
        if (where && !where->matches(row, registry))
            return {};
        Row output;
        output.reserve(projection.size());
        for (const auto& expression : projection)
            output.push_back(expression.evaluate(row, registry));
        return output;
    };
    if (query.table.empty()) {
        const Row empty;
        if (auto row = project(RowView(empty, 1)))
            co_yield std::move(*row);
        co_return;
    }
    Row null_right;
    if (join && join->kind == JoinKind::left)
        for (const auto& c : scope.right->columns)
            null_right.push_back(Null(c.type));
    const Value* primary_key = nullptr;
    if (!join && left.primary && where) {
        const BoundPredicate* guard = &*where;
        while (guard->kind == Predicate::Kind::all && !guard->children.empty())
            guard = &guard->children.front();
        const auto* direct = guard->direct_comparison();
        if (direct && direct->operation == Compare::equal && direct->left.index == left.primary->column &&
            !is_null(direct->right.value))
            primary_key = &direct->right.value;
    }
    std::optional<std::size_t> join_key;
    if (join && !right_where && scope.right->primary && on && on->leaf &&
        on->leaf->operation == Compare::equal) {
        const auto* a = &on->leaf->left;
        const auto* b = &on->leaf->right;
        if (a->index >= left.columns.size())
            std::swap(a, b);
        if (a->kind == Expr::Kind::column && b->kind == Expr::Kind::column &&
            a->index < left.columns.size() && b->index == left.columns.size() + scope.right->primary->column)
            join_key = a->index;
    }
    auto left_rows = source_rows(left, primary_key);
    while (left_rows.advance()) {
        auto a = left_rows.value();
        if (source_where && !source_where->matches(a, registry))
            continue;
        if (!join) {
            if (auto row = project(a))
                co_yield std::move(*row);
            continue;
        }
        bool matched = false;
        const Value* key = join_key ? &a[*join_key] : nullptr;
        auto right_rows = source_rows(*scope.right, key);
        while ((!key || !is_null(*key)) && right_rows.advance()) {
            auto b = right_rows.value();
            if (right_where && !right_where->matches(b, registry))
                continue;
            RowView pair(*a.left, *b.left, a.id);
            query_step();
            if (on && !on->matches(pair, registry))
                continue;
            matched = true;
            if (auto row = project(pair))
                co_yield std::move(*row);
        }
        if (!matched && join->kind == JoinKind::left)
            if (auto row = project(RowView(*a.left, null_right, a.id)))
                co_yield std::move(*row);
    }
}

Generator<Row> rows(const Tables& tables, Query query, const Registry& registry) {
    if (!query.limit)
        co_return;
    const auto limit = query.limit;
    auto remaining_offset = query.offset;
    std::size_t emitted = 0;
    auto compounds = std::move(query.compounds);
    query.compounds.clear();
    query.offset = 0;
    query.limit = std::numeric_limits<std::size_t>::max();
    auto input = scan(tables, query, registry);
    while (input.advance()) {
        if (remaining_offset) {
            --remaining_offset;
            continue;
        }
        co_yield std::move(input.value());
        if (++emitted == limit)
            co_return;
    }
    for (const auto& [operation, arm] : compounds) {
        (void)operation;
        auto input = rows(tables, *arm, registry);
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
        validate_streamable(query);
        auto shape = query;
        shape.limit = 0;
        shape.offset = 0;
        types = run(tables, shape, registry).types;
        for (const auto& [name, table] : tables) {
            (void)name;
            counters.snapshot_payload_bytes += table->payload_bytes;
        }
        input = rows(tables, query, registry);
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
