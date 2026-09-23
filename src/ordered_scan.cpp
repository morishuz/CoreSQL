#include "ordered_scan.hpp"

namespace coresql::detail::execution {
namespace {
bool collect_keys(const BoundPredicate& predicate, const Table& table, const Registry& registry,
                  std::vector<BoundLeaf>& leaves) {
    if (predicate.kind == Predicate::Kind::all) {
        for (const auto& child : predicate.children)
            if (!collect_keys(child, table, registry, leaves))
                return false;
        return true;
    }
    auto leaf = native_column_literal(table, predicate, registry);
    if (!leaf)
        return false;
    leaves.push_back(std::move(*leaf));
    return true;
}
} // namespace
std::optional<OrderedScanPlan> ordered_scan_plan(const Table& table, const Query& query,
                                                 const std::optional<BoundPredicate>& predicate,
                                                 const Registry& registry,
                                                 std::span<const BoundExpr> projection) {
    if (query.order_by.empty() || table.ordered.empty())
        return {};
    std::vector<BoundLeaf> leaves;
    if (predicate && !collect_keys(*predicate, table, registry, leaves))
        return {};
    Scope scope(table);
    scope.left_alias = query.alias;
    std::vector<BoundExpr> order;
    for (const auto& key : query.order_by) {
        if (key.expression.kind != Expr::Kind::column)
            return {};
        auto bound = bind(key.expression, scope, registry);
        if (!native_scalar(registry.addon(bound.type)))
            return {};
        order.push_back(std::move(bound));
    }
    auto equal = [&](std::size_t column) -> const Value* {
        for (const auto& leaf : leaves)
            if (leaf.left.index == column && leaf.operation == Compare::equal &&
                !table.columns[column].nullable && !is_null(leaf.right.value))
                return &leaf.right.value;
        return nullptr;
    };
    std::optional<OrderedScanPlan> chosen;
    std::size_t best = 0;
    for (const auto& index : table.ordered) {
        OrderedScanPlan plan;
        plan.index = index.get();
        for (auto column : index->columns) {
            if (auto value = equal(column))
                plan.bounds.prefix.push_back(*value);
            else {
                for (const auto& leaf : leaves) {
                    if (leaf.left.index != column || table.columns[column].nullable ||
                        is_null(leaf.right.value))
                        continue;
                    const auto& value = leaf.right.value;
                    if (leaf.operation == Compare::greater || leaf.operation == Compare::greater_equal)
                        if (!plan.bounds.lower || leaf.comparison.compare(value, *plan.bounds.lower) > 0)
                            plan.bounds.lower = value;
                    if (leaf.operation == Compare::less || leaf.operation == Compare::less_equal)
                        if (!plan.bounds.upper || leaf.comparison.compare(value, *plan.bounds.upper) < 0)
                            plan.bounds.upper = value;
                }
                break;
            }
        }
        std::size_t position = plan.bounds.prefix.size();
        std::optional<bool> reverse;
        bool matched = true;
        for (std::size_t i = 0; i < order.size(); ++i) {
            auto found = std::find(index->columns.begin(), index->columns.end(), order[i].index);
            if (found == index->columns.end()) {
                matched = false;
                break;
            }
            auto key = static_cast<std::size_t>(found - index->columns.begin());
            plan.order_columns.push_back(key);
            plan.comparisons.emplace_back(registry, order[i].type, false);
            if (equal(order[i].index))
                continue;
            if (key != position++) {
                matched = false;
                break;
            }
            const bool desc = !index->definition.descending.empty() && index->definition.descending[key];
            const bool flip = desc != query.order_by[i].descending;
            if (reverse && *reverse != flip) {
                matched = false;
                break;
            }
            reverse = flip;
        }
        if (!matched)
            continue;
        plan.reverse = reverse.value_or(false);
        plan.row_columns.resize(table.columns.size(), std::numeric_limits<std::size_t>::max());
        for (std::size_t i = 0; i < index->columns.size(); ++i)
            plan.row_columns[index->columns[i]] = i;
        auto covered = [&](auto&& self, const BoundExpr& expression) -> bool {
            if (expression.kind == Expr::Kind::row_id)
                return false;
            if (expression.kind == Expr::Kind::column)
                return plan.row_columns[expression.index] != std::numeric_limits<std::size_t>::max();
            return std::all_of(expression.arguments.begin(), expression.arguments.end(),
                               [&](const auto& argument) { return self(self, argument); });
        };
        const bool covering =
            std::all_of(projection.begin(), projection.end(),
                        [&](const auto& expression) { return covered(covered, expression); }) &&
            std::all_of(leaves.begin(), leaves.end(),
                        [&](const auto& leaf) { return covered(covered, leaf.left); });
        // Prefer coverage when bounds are equally selective; do not discard a
        // longer equality prefix merely to avoid fetching the surviving rows.
        auto score =
            (1 + plan.bounds.prefix.size() * 2 + (plan.bounds.lower || plan.bounds.upper ? 1 : 0)) * 2 +
            covering;
        if (score > best) {
            best = score;
            chosen = std::move(plan);
        }
    }
    return chosen;
}
bool OrderedScan::same_order(const Row& a, const Row& b) const {
    for (std::size_t i = 0; i < plan_.order_columns.size(); ++i) {
        auto column = plan_.order_columns[i];
        if (plan_.comparisons[i].compare(a[column], b[column]))
            return false;
    }
    return true;
}
std::optional<OrderedIndex::Entry> OrderedScan::read() {
    while (auto entry = walk_.next_entry()) {
#ifdef CORESQL_TESTING
        if (auto* counters = detail::active_query_counters)
            ++counters->ordered_entries;
#endif
        if (!table_.selection ||
            std::binary_search(table_.selection->begin(), table_.selection->end(), entry->location))
            return entry;
    }
    return {};
}
std::optional<OrderedIndex::Entry> OrderedScan::next() {
    QueryBuffer memory;
    memory.add(buffer_bytes());
    if (offset_ < group_.size())
        return group_[offset_++];
    group_.clear();
    offset_ = 0;
    if (!pending_)
        pending_ = read();
    if (!pending_)
        return {};
    const auto key = pending_->key;
    do {
        if (group_.size() == group_.capacity()) {
            if (group_.capacity() > group_.max_size() / 2)
                fail(ErrorCode::resource, "Ordered tie buffer size overflow");
            auto capacity = std::max(std::size_t{1}, group_.capacity() * 2);
            // Charge both allocations while reserve replaces the previous one.
            memory.add(capacity * sizeof(OrderedIndex::Entry));
            auto old = group_.capacity();
            group_.reserve(capacity);
            memory.remove(old * sizeof(OrderedIndex::Entry));
        }
        group_.push_back(std::move(*pending_));
        pending_ = read();
    } while (pending_ && same_order(*key, *pending_->key));
    std::sort(group_.begin(), group_.end(), [](const auto& a, const auto& b) {
        query_step();
        return a.location < b.location;
    });
    return group_[offset_++];
}
} // namespace coresql::detail::execution
