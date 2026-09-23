#pragma once
#include "bound.hpp"
#include "ordered_index.hpp"

namespace coresql::detail::execution {
struct OrderedScanPlan {
    const OrderedIndex* index = nullptr;
    OrderedIndex::Bounds bounds;
    bool reverse = false;
    std::vector<std::size_t> order_columns, row_columns;
    std::vector<Comparison> comparisons;
};
// Only total native predicates and column orderings may stop a materialized
// scan early: repeatability alone does not certify the absence of errors.
std::optional<OrderedScanPlan> ordered_scan_plan(const Table&, const Query&,
                                                 const std::optional<BoundPredicate>&, const Registry&,
                                                 std::span<const BoundExpr> projection);

// Shares bounded traversal and stable ORDER BY ties across both executors.
// Account retained storage inside each call, never across coroutine suspension.
class OrderedScan {
    const Table& table_;
    const OrderedScanPlan& plan_;
    OrderedIndex::Walk walk_;
    std::optional<OrderedIndex::Entry> pending_;
    std::vector<OrderedIndex::Entry> group_;
    std::size_t offset_ = 0;
    bool same_order(const Row&, const Row&) const;
    std::optional<OrderedIndex::Entry> read();

public:
    OrderedScan(const Table& table, const OrderedScanPlan& plan)
        : table_(table), plan_(plan), walk_(*plan.index, plan.reverse, &plan.bounds) {}
    std::optional<OrderedIndex::Entry> next();
    std::size_t buffer_bytes() const {
        return walk_.buffer_bytes() + group_.capacity() * sizeof(OrderedIndex::Entry);
    }
};
} // namespace coresql::detail::execution
