#pragma once
#include "scan.hpp"
#include "ordered_index.hpp"

namespace coresql::detail::execution {
// Read original indexed values directly; fetch the immutable table row only
// for a missing column or logical row identity. A physical location is not an ID.
class IndexRow {
    const Table& table_;
    const OrderedIndex::Entry& entry_;
    const std::vector<std::size_t>& columns_;
    mutable std::optional<PinnedRow> pinned_;
    const PinnedRow& pin() const {
        if (!pinned_) {
            pinned_ = indexed_row(table_, entry_.location);
#ifdef CORESQL_TESTING
            ++detail::visited_chunks();
            if (auto* counters = detail::active_query_counters)
                ++counters->ordered_row_fetches;
#endif
        }
        return *pinned_;
    }

public:
    IndexRow(const Table& table, const OrderedIndex::Entry& entry, const std::vector<std::size_t>& columns)
        : table_(table), entry_(entry), columns_(columns) {}
    const Value& operator[](std::size_t column) const {
        auto key = columns_[column];
        if (key != std::numeric_limits<std::size_t>::max())
            return (*entry_.key)[key];
        const auto& row = pin();
        return row.chunk->rows[row.position][column];
    }
    std::int64_t identity() const {
        const auto& row = pin();
        return row.chunk->rowids[row.position];
    }
};
} // namespace coresql::detail::execution
