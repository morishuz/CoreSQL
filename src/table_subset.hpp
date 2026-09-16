#pragma once
#include "state.hpp"

namespace coresql::detail::execution {
// A read-only row selection, retaining immutable source chunks and logical IDs.
// Physical indexes are not inherited: they could expose excluded rows.
inline std::shared_ptr<Table> table_subset(const Table& table, std::span<const RowLocation> selected) {
    auto result = std::make_shared<Table>();
    result->columns = table.columns;
    result->next_rowid = table.next_rowid;
    result->row_count = selected.size();
    result->selection.emplace(selected.begin(), selected.end());
    for (auto location : selected)
        if (!result->chunks.contains(location.chunk)) {
            auto chunk = table.chunks.find(location.chunk);
            if (chunk == table.chunks.end())
                throw Error(ErrorCode::state, "Index returned a missing chunk");
            result->chunks.emplace(location.chunk, chunk->second);
        }
    return result;
}
} // namespace coresql::detail::execution
