#pragma once
#include "bound.hpp"

namespace coresql::detail::execution {
// Retain the final WHERE and physical join order. Only a leading, non-failing
// native scalar comparison prefix can reject rows before intermediate materialization.
inline std::vector<Predicate> native_join_prefix(const Tables& tables, const Query& query,
                                                 const Registry& registry) {
    if (!query.limit || !query.repeatable || !query.where)
        return {};
    std::map<std::string, std::string> sources{{query.alias, query.table}};
    for (const auto& join : query.joins) {
        if (join.kind != JoinKind::inner || join.on || join.right_where)
            return {};
        sources.emplace(join.alias, join.table);
    }
    auto operand_type = [&](const Expr& e) -> std::optional<Type> {
        if (e.kind == Expr::Kind::literal)
            return type_of(e.value);
        if (e.kind != Expr::Kind::column || !sources.contains(e.qualifier))
            return {};
        const auto& columns = require_table(tables, sources.at(e.qualifier))->columns;
        for (const auto& c : columns)
            if (c.name == e.name)
                return c.type;
        return {};
    };
    auto i64_operand = [&](const Expr& e) {
        auto type = operand_type(e);
        return type && native_i64(registry.addon(*type));
    };
    auto native_operand = [&](const Type& type) { return native_scalar(registry.addon(type)); };
    // Earlier pruning must not skip a potentially failing later ON operation.
    for (const auto& join : query.joins)
        if (!join.cross && (!i64_operand(join.left) || !i64_operand(join.right)))
            return {};
    std::vector<Predicate> result;
    std::function<bool(const Predicate&)> collect = [&](const Predicate& p) {
        if (p.kind == Predicate::Kind::all) {
            for (const auto& child : p.children)
                if (!collect(child))
                    return false;
            return true;
        }
        auto left = operand_type(p.left), right = operand_type(p.right);
        if (p.kind != Predicate::Kind::comparison || left != right || !left || !native_operand(*left))
            return false;
        result.push_back(p);
        return true;
    };
    const bool complete = collect(*query.where);
    // With a source filter, only the complete native i64 column predicate
    // is eligible, and consumers must apply it after that filter has completed.
    if (query.source_where &&
        (!complete || !std::all_of(result.begin(), result.end(), [&](const Predicate& p) {
            return p.left.kind == Expr::Kind::column && p.right.kind == Expr::Kind::column &&
                   i64_operand(p.left) && i64_operand(p.right);
        })))
        return {};
    return result;
}

template <class Available>
std::optional<Predicate> available_join_prefix(const std::vector<Predicate>& prefix, Available available) {
    std::vector<Predicate> result;
    auto ready = [&](const Expr& e) { return e.kind == Expr::Kind::literal || available(e); };
    for (const auto& p : prefix)
        if (ready(p.left) && ready(p.right))
            result.push_back(p);
    if (result.empty())
        return {};
    return all_of(std::move(result));
}
} // namespace coresql::detail::execution
