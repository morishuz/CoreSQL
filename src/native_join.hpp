#pragma once
#include "integer_join.hpp"
#include "integer_join_positions.hpp"
#include <iterator>

namespace coresql::detail::execution {
inline bool native_join_key(const TypeAddon& addon) {
    return native_i64(addon) ||
           (addon.native_ops && addon.hash && addon.equal &&
            (addon.layout == Layout::text || addon.layout == Layout::f64 || addon.layout == Layout::i128));
}

// Keys borrow validated values in the retained right-hand snapshot. No text or
// opaque payload is copied. The certificate excludes custom comparison semantics.
// Hash collisions are resolved by the same registered equality used by the join.
template <class Position> class NativeJoinBuckets {
    struct Hash {
        const TypeAddon* addon;
        Bytes parameters;
        std::size_t operator()(const Value* key) const { return addon->hash(parameters, *key); }
    };
    struct Equal {
        const TypeAddon* addon;
        Bytes parameters;
        bool operator()(const Value* a, const Value* b) const { return addon->equal(parameters, *a, *b); }
    };
    using Map = std::unordered_map<const Value*, std::vector<Position>, Hash, Equal>;
    std::optional<Map> buckets;

public:
    bool built() const { return buckets.has_value(); }
    void create(const Registry& registry, const Type& type) {
        const auto* addon = &registry.addon(type);
        buckets.emplace(0, Hash{addon, type.parameters}, Equal{addon, type.parameters});
    }
    void add(const Value& key, Position position) { (*buckets)[&key].push_back(position); }
    const std::vector<Position>& find(const Value& key) const {
        static const std::vector<Position> empty;
        const auto found = buckets->find(&key);
        return found == buckets->end() ? empty : found->second;
    }
};
class NativeJoinLookup {
    IntegerJoinLookup integers;
    NativeJoinBuckets<const Row*> keys;

public:
    const std::vector<const Row*>& find(const Table& table, std::size_t column, const Value& key,
                                        const Registry& registry, const Type& type) {
        if (native_i64(registry.addon(type)))
            return integers.find(table, column, i64_payload(key));
        if (!keys.built()) {
            keys.create(registry, type);
            visit_table_rows(table, [&](auto, const Row& row, auto) {
#ifdef CORESQL_TESTING
                if (auto* counters = active_query_counters)
                    ++counters->hash_build_rows;
#endif
                if (!is_null(row[column]))
                    keys.add(row[column], &row);
                return true;
            });
        }
#ifdef CORESQL_TESTING
        if (auto* counters = active_query_counters)
            ++counters->hash_probes;
#endif
        return keys.find(key);
    }
};
class NativeJoinPositions {
    IntegerJoinPositions integers;
    NativeJoinBuckets<std::size_t> keys;
    std::vector<std::size_t> nulls, merged;

public:
    const std::vector<std::size_t>& find(const std::vector<const Row*>& rows, std::size_t column,
                                         const Value& key, const Registry& registry, const Type& type) {
        if (native_i64(registry.addon(type)))
            return integers.find(rows, column, i64_payload(key));
        if (!keys.built()) {
            keys.create(registry, type);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const auto& value = (*rows[i])[column];
                if (is_null(value))
                    nulls.push_back(i);
                else
                    keys.add(value, i);
#ifdef CORESQL_TESTING
                if (auto* counters = active_query_counters)
                    ++counters->hash_build_rows;
#endif
            }
        }
#ifdef CORESQL_TESTING
        if (auto* counters = active_query_counters)
            ++counters->hash_probes;
#endif
        const auto& found = keys.find(key);
        if (nulls.empty())
            return found;
        // UNKNOWN must still reach later ON expressions, in nested-loop order.
        merged.clear();
        merged.reserve(nulls.size() + found.size());
        std::merge(nulls.begin(), nulls.end(), found.begin(), found.end(), std::back_inserter(merged));
        return merged;
    }
};
} // namespace coresql::detail::execution
