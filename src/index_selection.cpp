#include "bound.hpp"
#include "ordered_index.hpp"

namespace coresql::detail::execution {
std::optional<IndexResult> composite_candidates(const Table& table,
                                                const std::optional<BoundPredicate>& predicate,
                                                const Registry& registry) {
    if (!predicate || table.ordered.empty())
        return {};
    std::vector<const BoundLeaf*> prefix;
    // Only cross total native comparisons on non-null columns. A later key
    // must not hide an earlier callback error or SQL UNKNOWN's evaluation.
    auto collect = [&](auto&& self, const BoundPredicate& p) -> bool {
        if (p.kind == Predicate::Kind::all) {
            for (const auto& child : p.children)
                if (!self(self, child))
                    return false;
            return true;
        }
        const auto* leaf = p.direct_comparison();
        if (!leaf || leaf->left.index >= table.columns.size() || is_null(leaf->right.value) ||
            table.columns[leaf->left.index].nullable || !native_scalar(registry.addon(leaf->left.type)))
            return false;
        prefix.push_back(leaf);
        return true;
    };
    collect(collect, *predicate);
    const OrderedIndex* chosen = nullptr;
    Row keys;
    const Value *low = nullptr, *high = nullptr;
    std::size_t best = 0;
    for (const auto& index : table.ordered) {
        Row equal;
        const Value *lo = nullptr, *hi = nullptr;
        for (auto column : index->columns) {
            const Value* value = nullptr;
            for (const auto* leaf : prefix)
                if (leaf->left.index == column && leaf->operation == Compare::equal) {
                    value = &leaf->right.value;
                    break;
                }
            if (value) {
                equal.push_back(*value);
                continue;
            }
            for (const auto* leaf : prefix)
                if (leaf->left.index == column) {
                    // Inclusive candidates also cover strict bounds; the
                    // original predicate still rejects the boundary rows.
                    if (leaf->operation == Compare::greater_equal || leaf->operation == Compare::greater)
                        lo = &leaf->right.value;
                    if (leaf->operation == Compare::less_equal || leaf->operation == Compare::less)
                        hi = &leaf->right.value;
                }
            break;
        }
        const auto score = equal.size() * 2 + (lo || hi ? 1 : 0);
        if (score > best) {
            best = score;
            chosen = index.get();
            keys = std::move(equal);
            low = lo;
            high = hi;
        }
    }
    if (!chosen)
        return {};
    return IndexResult{chosen->prefix_range(keys, low, high), false};
}
} // namespace coresql::detail::execution
