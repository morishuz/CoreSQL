#pragma once
#include "bound.hpp"

namespace coresql::detail::execution {
// Keep predicate position and lazy evaluation unchanged. A repeatable Cartesian
// join can reuse its leading left-only predicates for subsequent right rows.
// UNKNOWN remains UNKNOWN; failures are never stored. One row is retained by
// identity, not copied, and the snapshot owns it for this scan's lifetime.
inline void reuse_left_predicates(BoundPredicate& predicate, std::size_t width) {
    auto left_expression = [&](auto&& self, const BoundExpr& e) -> bool {
        if (e.kind == Expr::Kind::column && e.index >= width)
            return false;
        return std::all_of(e.arguments.begin(), e.arguments.end(),
                           [&](const auto& a) { return self(self, a); });
    };
    auto left_predicate = [&](auto&& self, const BoundPredicate& p) -> bool {
        if (p.leaf)
            return left_expression(left_expression, p.leaf->left) &&
                   left_expression(left_expression, p.leaf->right);
        if (p.membership)
            return left_expression(left_expression, p.membership->left) &&
                   left_expression(left_expression, p.membership->right);
        return std::all_of(p.children.begin(), p.children.end(),
                           [&](const auto& child) { return self(self, child); });
    };
    auto prefix = [&](auto&& self, BoundPredicate& p) -> bool {
        if (p.kind == Predicate::Kind::all) {
            for (auto& child : p.children)
                if (!self(self, child))
                    return false;
            return true;
        }
        if (!left_predicate(left_predicate, p))
            return false;
        p.reused = std::make_unique<BoundPredicate::Memo>();
        return true;
    };
    prefix(prefix, predicate);
}
} // namespace coresql::detail::execution
