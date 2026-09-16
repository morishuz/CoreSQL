#pragma once
#include "coresql/core.hpp"

namespace coresql::detail::execution {
class MembershipLookup {
    bool attempted = false, has_null = false, empty = true;
    std::function<bool(const Value&)> contains;

public:
    template <class Rows, class Extract>
    void prepare(const Function& function, const Type& type, const Rows& rows, Extract extract) {
        if (attempted || !function.prepare_membership)
            return;
        attempted = true;
        std::vector<Value> candidates;
        candidates.reserve(rows.size());
        for (const auto& row : rows) {
            empty = false;
            const auto& value = extract(row);
            if (is_null(value))
                has_null = true;
            else
                candidates.push_back(value);
        }
        contains = function.prepare_membership(type, candidates);
    }
    std::optional<Value> find(const Value& value) const {
        if (!contains)
            return {};
        if (empty)
            return Value(std::int64_t{0});
        if (is_null(value))
            return Value(Null(integer()));
        if (contains(value))
            return Value(std::int64_t{1});
        return has_null ? Value(Null(integer())) : Value(std::int64_t{0});
    }
};
} // namespace coresql::detail::execution
