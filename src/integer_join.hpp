#pragma once
#include "scan.hpp"
#include <unordered_map>
#include <array>

namespace coresql::detail::execution {
// Query-local, lazy lookup. Borrowed rows stay alive through the query snapshot.
// Bucket insertion order preserves the nested-loop order for duplicate keys.
class IntegerJoinLookup {
    using Buckets = std::unordered_map<std::int64_t, std::vector<const Row*>>;
    std::optional<Buckets> buckets;
    // A bounded negative filter avoids hash-table probes for sparse key sets.
    // Collisions always fall through to the exact lookup.
    std::array<std::uint64_t, 4> presence{};
    bool selective = false;
    static std::size_t bit(std::int64_t key) { return static_cast<std::uint64_t>(key) & 255; }

public:
    const std::vector<const Row*>& find(const Table& table, std::size_t column, std::int64_t key) {
        if (!buckets) {
            buckets.emplace();
            visit_table_rows(table, [&](auto, const Row& row, auto) {
#ifdef CORESQL_TESTING
                if (auto* counters = active_query_counters)
                    ++counters->hash_build_rows;
#endif
                if (!is_null(row[column]))
                    (*buckets)[std::get<std::int64_t>(row[column])].push_back(&row);
                return true;
            });
            selective = buckets->size() <= 128;
            if (selective) {
                for (const auto& [value, rows] : *buckets) {
                    auto position = bit(value);
                    presence[position / 64] |= std::uint64_t{1} << (position % 64);
                }
            }
        }
#ifdef CORESQL_TESTING
        if (auto* counters = active_query_counters)
            ++counters->hash_probes;
#endif
        static const std::vector<const Row*> empty;
        auto position = bit(key);
        if (selective && !(presence[position / 64] & (std::uint64_t{1} << (position % 64))))
            return empty;
        const auto found = buckets->find(key);
        return found == buckets->end() ? empty : found->second;
    }
};
} // namespace coresql::detail::execution
