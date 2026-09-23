#include "bound.hpp"
#include "ordered_index.hpp"
#include "index.hpp"

namespace coresql::detail::execution {
namespace {
std::optional<BoundLeaf> native_key(const Table& table, const BoundPredicate& predicate,
                                    const Registry& registry) {
    if (!predicate.leaf)
        return {};
    const auto& left = predicate.leaf->left;
    const auto& right = predicate.leaf->right;
    if (!((left.kind == Expr::Kind::column && right.kind == Expr::Kind::literal) ||
          (left.kind == Expr::Kind::literal && right.kind == Expr::Kind::column)))
        return {};
    auto leaf = *predicate.leaf;
    if (leaf.left.kind == Expr::Kind::literal && leaf.right.kind == Expr::Kind::column) {
        std::swap(leaf.left, leaf.right);
        switch (leaf.operation) {
        case Compare::less:
            leaf.operation = Compare::greater;
            break;
        case Compare::less_equal:
            leaf.operation = Compare::greater_equal;
            break;
        case Compare::greater:
            leaf.operation = Compare::less;
            break;
        case Compare::greater_equal:
            leaf.operation = Compare::less_equal;
            break;
        default:
            break;
        }
    }
    if (leaf.left.kind != Expr::Kind::column || leaf.right.kind != Expr::Kind::literal ||
        leaf.left.index >= table.columns.size() || is_null(leaf.right.value) ||
        table.columns[leaf.left.index].nullable || !native_scalar(registry.addon(leaf.left.type)))
        return {};
    return leaf;
}
} // namespace

std::optional<IndexResult> primary_range_candidates(const Table& table,
                                                    const std::optional<BoundPredicate>& predicate,
                                                    const Registry& registry) {
    if (!table.primary || !predicate || !registry.addon(table.columns[table.primary->column].type).compare)
        return {};
    std::optional<Value> lower, upper;
    auto collect = [&](auto&& self, const BoundPredicate& p) -> bool {
        if (p.kind == Predicate::Kind::all) {
            for (const auto& child : p.children)
                if (!self(self, child))
                    return false;
            return true;
        }
        auto leaf = native_key(table, p, registry);
        if (!leaf)
            return false; // Do not cross callbacks or nullable comparisons.
        if (leaf->left.index == table.primary->column) {
            const auto& value = leaf->right.value;
            if (leaf->operation == Compare::equal || leaf->operation == Compare::greater ||
                leaf->operation == Compare::greater_equal)
                if (!lower || leaf->comparison.compare(value, *lower) > 0)
                    lower = value;
            if (leaf->operation == Compare::equal || leaf->operation == Compare::less ||
                leaf->operation == Compare::less_equal)
                if (!upper || leaf->comparison.compare(value, *upper) < 0)
                    upper = value;
        }
        return true;
    };
    collect(collect, *predicate);
    if (!lower && !upper)
        return {};
    auto result = table.primary->data->range(lower ? &*lower : nullptr, upper ? &*upper : nullptr);
    // Bounds are inclusive candidates, even when the original comparison is strict.
    if (result)
        result->exact = false;
    return result;
}

std::optional<IndexResult> join_guard_candidates(const Table& table, const BoundPredicate& guard,
                                                 const Registry& registry) {
    if (guard.kind == Predicate::Kind::any) {
        IndexResult result;
        for (const auto& child : guard.children) {
            auto selected = join_guard_candidates(table, child, registry);
            if (!selected)
                return {}; // An unindexed arm may retain any row, including UNKNOWN.
            result.rows.insert(result.rows.end(), selected->rows.begin(), selected->rows.end());
        }
        normalize(result);
        return result;
    }
    std::optional<BoundPredicate> keys{BoundPredicate{Predicate::Kind::all, {}, {}, {}}};
    auto collect = [&](auto&& self, const BoundPredicate& p) -> void {
        if (p.kind == Predicate::Kind::all) {
            for (const auto& child : p.children)
                self(self, child);
        } else if (auto leaf = native_key(table, p, registry)) {
            // These conjuncts cannot be UNKNOWN. Nullable guards remain in the
            // residual; discarding their NULL keys would change error behavior.
            keys->children.push_back(BoundPredicate{Predicate::Kind::comparison, *leaf, {}, {}});
        }
    };
    collect(collect, guard);
    if (keys->children.empty())
        return {};
    if (table.primary) {
        auto point = std::find_if(keys->children.begin(), keys->children.end(), [&](const auto& p) {
            return p.leaf->left.index == table.primary->column && p.leaf->operation == Compare::equal;
        });
        if (point != keys->children.end())
            std::iter_swap(keys->children.begin(), point);
    }
    // candidates may consume an exact guard, but only this disposable copy.
    auto result = candidates(table, keys, registry);
    if (result)
        normalize(*result);
    return result;
}

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
