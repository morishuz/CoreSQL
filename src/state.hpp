#pragma once
#include "coresql/core.hpp"
#include "chunk_map.hpp"
#include <algorithm>
#include <mutex>

namespace coresql::detail {
template <class Tables> auto& require_table(Tables& tables, const std::string& name) {
    auto found = tables.find(name);
    if (found == tables.end())
        throw Error(ErrorCode::schema, "Unknown table: " + name);
    return found->second;
}

// Chunk limits bound normal copy-on-write work; a single large value may exceed
// the byte target. Published chunks are never modified through another snapshot.
inline constexpr std::size_t chunk_rows = 128;
inline constexpr std::size_t chunk_bytes = 16 * 1024;
struct Chunk {
    std::vector<Row> rows;
    std::vector<std::int64_t> rowids;
    std::size_t payload_bytes = 0, encoded_bytes = 0;
    // Empty means identity mapping. Only compacted chunks need slot metadata.
    // Slots are snapshot-local and regenerated when persisted rows are loaded.
    std::vector<std::uint32_t> slots;
    std::uint32_t slot(std::size_t position) const {
        return slots.empty() ? static_cast<std::uint32_t>(position) : slots[position];
    }
    std::uint64_t next_slot() const { return slots.empty() ? rows.size() : std::uint64_t(slots.back()) + 1; }
    std::size_t position(std::uint32_t slot) const {
        if (slots.empty())
            return slot < rows.size() ? slot : rows.size();
        auto found = std::lower_bound(slots.begin(), slots.end(), slot);
        return found != slots.end() && *found == slot ? static_cast<std::size_t>(found - slots.begin())
                                                      : rows.size();
    }
};
// Schema validation is shared by mutation, recovery and integrity checking.
void validate_stored(const Value&, const Column&, const Registry&);
void prepare_row(Row&, const std::vector<Column>&, const Registry&);

struct IndexBinding;
class OrderedIndex;
struct Table {
    // Query-local views share immutable source chunks and restrict visible rows.
    // Published database tables never carry a selection or inherit view indexes.
    std::optional<std::vector<RowLocation>> selection;
    std::vector<Column> columns;
    TableConstraints constraints;
    ChunkMap chunks;
    std::uint64_t next_chunk = 0;
    std::int64_t next_rowid = 1;
    std::size_t row_count = 0, payload_bytes = 0;
    std::shared_ptr<IndexBinding> primary;
    std::vector<std::shared_ptr<IndexBinding>> indexes;
    std::vector<IndexDefinition> index_definitions;
    std::vector<std::shared_ptr<OrderedIndex>> ordered;
};
using Tables = std::map<std::string, std::shared_ptr<Table>>;
void validate_constraints(const Tables&, const Registry&, const std::string& changed = {});
void validate_insert_constraints(const Tables&, const std::string&, const Row&, const Registry&);
void validate_replacement_constraints(const Tables&, const std::string&, const std::shared_ptr<Table>&,
                                      const Registry&);
void validate_check_expression(const Expr&);
struct State {
    // Process-local schema epoch: destructive DDL commits use atomic checkpoints.
    std::shared_ptr<const int> schema_epoch;
    Tables tables;
    Stats stats;
};
class DurableStore;
class Pager;
struct Owner {
    Registry registry;
    std::shared_ptr<const State> current;
    std::shared_ptr<DurableStore> storage;
    std::shared_ptr<Pager> pager;
    OpenOptions options;
    std::shared_ptr<const Registry> read_registry;
    mutable std::mutex state_mutex;
    std::mutex commit_mutex;
    explicit Owner(Registry r, OpenOptions o)
        : registry(std::move(r)), current(std::make_shared<const State>()), options(o) {
        if (options.concurrent_reads)
            read_registry = std::make_shared<const Registry>(registry);
    }
    std::shared_ptr<const State> capture() const {
        std::lock_guard lock(state_mutex);
        return current;
    }
    void publish(std::shared_ptr<const State> next) {
        std::lock_guard lock(state_mutex);
        current = std::move(next);
    }
};
#ifdef CORESQL_TESTING
struct QueryStageStats {
    const char* kind;
    std::size_t rows, columns, row_capacity_bytes, match_capacity_bytes;
};
struct QueryCounters {
    std::vector<QueryStageStats> stages;
    std::size_t rows_tested = 0, candidate_pairs = 0, intermediate_rows = 0;
    std::size_t largest_intermediate = 0, subquery_executions = 0;
    std::size_t hash_build_rows = 0, hash_probes = 0;
    std::size_t correlation_index_rows = 0, subquery_cache_hits = 0;
};
extern thread_local QueryCounters* active_query_counters;
// Container capacity only: excludes dynamic payloads, indexes and allocator overhead.
inline void record_query_stage(const char* kind, const Result& result, std::size_t matches = 0) {
    if (auto* counters = active_query_counters) {
        std::size_t bytes = result.rows.capacity() * sizeof(Row);
        for (const auto& row : result.rows)
            bytes += row.capacity() * sizeof(Value);
        counters->stages.push_back({kind, result.rows.size(), result.types.size(), bytes, matches});
    }
}
class QueryCounterScope {
    QueryCounters* previous;

public:
    explicit QueryCounterScope(QueryCounters& counters) : previous(active_query_counters) {
        active_query_counters = &counters;
    }
    ~QueryCounterScope() { active_query_counters = previous; }
    QueryCounterScope(const QueryCounterScope&) = delete;
    QueryCounterScope& operator=(const QueryCounterScope&) = delete;
};
// Per-thread diagnostics for access-path regression tests; absent in production.
std::size_t& visited_chunks();
std::size_t& retained_matches();
#endif
} // namespace coresql::detail
