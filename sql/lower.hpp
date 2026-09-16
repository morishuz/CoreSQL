#pragma once
#include "ast.hpp"

// Shared SQL lowering rules; independent of core execution/storage internals.
namespace coresql::sql::detail {
[[noreturn]] inline void unsupported(const std::string& s) {
    throw Error(ErrorCode::unsupported, s);
}
inline const std::vector<Column>& table(const Schema& schema, const std::string& name) {
    auto it = schema.find(name);
    if (it == schema.end())
        throw Error(ErrorCode::schema, "Unknown SQL table: " + name);
    return it->second;
}
inline std::optional<Compare> comparison(const std::string& s) {
    if (s == "=" || s == "==")
        return Compare::equal;
    if (s == "!=" || s == "<>")
        return Compare::not_equal;
    if (s == "<")
        return Compare::less;
    if (s == ">")
        return Compare::greater;
    if (s == "<=")
        return Compare::less_equal;
    if (s == ">=")
        return Compare::greater_equal;
    return {};
}
inline Predicate conjunction(std::vector<Predicate> input) {
    std::vector<Predicate> flat;
    for (auto& p : input) {
        if (p.kind == Predicate::Kind::all)
            for (auto& child : p.children)
                flat.push_back(std::move(child));
        else
            flat.push_back(std::move(p));
    }
    if (flat.size() == 1)
        return std::move(flat[0]);
    return all_of(std::move(flat));
}
} // namespace coresql::sql::detail
