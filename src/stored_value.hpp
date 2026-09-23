#pragma once
#include "coresql/core.hpp"
#include <bit>

namespace coresql::detail {
// Storage identity must preserve representations such as signed zero, even when
// a type's comparison callback considers them equal.
inline bool same_stored_value(const Value& a, const Value& b) {
    if (a.index() != b.index())
        return false;
    if (const auto* value = std::get_if<double>(&a))
        return std::bit_cast<std::uint64_t>(*value) == std::bit_cast<std::uint64_t>(std::get<double>(b));
    return a == b;
}
} // namespace coresql::detail
