#pragma once
#include "bound.hpp"

namespace coresql::detail::execution {
// Only projection-local, repeatable scalar calls share results. Slots are reset
// for each projected row and populated on first demand, including inside CASE.
// Predicates, sorting, subqueries and separate scans never share these slots.
class ProjectionCache {
    std::vector<std::shared_ptr<std::optional<Value>>> slots;
    static bool scalar(const BoundExpr& e) {
        if (e.kind == Expr::Kind::column || e.kind == Expr::Kind::literal || e.kind == Expr::Kind::row_id)
            return true;
        return e.kind == Expr::Kind::call && std::all_of(e.arguments.begin(), e.arguments.end(), scalar);
    }
    static bool same(const BoundExpr& a, const BoundExpr& b) {
        if (a.kind != b.kind || a.type != b.type || a.index != b.index || a.function != b.function ||
            a.arguments.size() != b.arguments.size())
            return false;
        if (a.kind == Expr::Kind::literal && a.value != b.value)
            return false;
        for (std::size_t i = 0; i < a.arguments.size(); ++i)
            if (!same(a.arguments[i], b.arguments[i]))
                return false;
        return true;
    }

public:
    ProjectionCache(std::vector<BoundExpr>& expressions, bool repeatable) {
        if (!repeatable)
            return;
        std::vector<BoundExpr*> seen;
        auto visit = [&](auto&& visit, BoundExpr& e) -> void {
            for (auto& a : e.arguments)
                visit(visit, a);
            // Copying a cached string can cost more than recomputing it.
            if (e.kind != Expr::Kind::call || e.type == text() || !scalar(e))
                return;
            for (auto* previous : seen) {
                if (!same(e, *previous))
                    continue;
                if (!previous->reused) {
                    previous->reused = std::make_shared<std::optional<Value>>();
                    slots.push_back(previous->reused);
                }
                e.reused = previous->reused;
                return;
            }
            seen.push_back(&e);
        };
        for (auto& e : expressions)
            visit(visit, e);
    }
    void reset() const {
        for (const auto& slot : slots)
            slot->reset();
    }
};
} // namespace coresql::detail::execution
