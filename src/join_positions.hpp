#pragma once
#include <algorithm>
#include <iterator>
#include <vector>

namespace coresql::detail::execution {
// Both lists are built in source order. Preserve that order so UNKNOWN keys
// reach subsequent ON expressions at the same point as in a nested-loop join.
inline const std::vector<std::size_t>& merge_join_positions(const std::vector<std::size_t>& matches,
                                                            const std::vector<std::size_t>& nulls,
                                                            std::vector<std::size_t>& scratch) {
    if (nulls.empty())
        return matches;
    if (matches.empty())
        return nulls;
    scratch.clear();
    scratch.reserve(matches.size() + nulls.size());
    std::merge(matches.begin(), matches.end(), nulls.begin(), nulls.end(), std::back_inserter(scratch));
    return scratch;
}
} // namespace coresql::detail::execution
