#pragma once
#include "state.hpp"
#include <unordered_map>

namespace coresql::detail::execution {
// Candidate positions for a leading equality in a general ON predicate. Include
// NULL keys, since UNKNOWN must still evaluate the rest of ON. Positions retain
// source order and let outer joins track actual matches independently of WHERE.
class IntegerJoinPositions {
    bool built = false;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> buckets;
    std::vector<std::size_t> nulls, merged;

public:
    const std::vector<std::size_t>& find(const std::vector<const Row*>& rows, std::size_t column,
                                         std::int64_t key) {
        if (!built) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const auto& value = (*rows[i])[column];
                if (is_null(value))
                    nulls.push_back(i);
                else
                    buckets[std::get<std::int64_t>(value)].push_back(i);
#ifdef CORESQL_TESTING
                if (auto* counters = active_query_counters)
                    ++counters->hash_build_rows;
#endif
            }
            built = true;
        }
#ifdef CORESQL_TESTING
        if (auto* counters = active_query_counters)
            ++counters->hash_probes;
#endif
        auto found = buckets.find(key);
        static const std::vector<std::size_t> empty;
        if (nulls.empty())
            return found == buckets.end() ? empty : found->second;
        merged = nulls;
        if (found != buckets.end()) {
            merged.insert(merged.end(), found->second.begin(), found->second.end());
            std::sort(merged.begin(), merged.end());
        }
        return merged;
    }
};
} // namespace coresql::detail::execution
