#pragma once
#include "bound.hpp"

namespace coresql::detail::execution {
inline const Row& indexed_row(const detail::Table& table, RowLocation location) {
    auto found = table.chunks.find(location.chunk);
    if (found == table.chunks.end())
        fail(ErrorCode::state, "Index returned a missing chunk");
    auto position = found->second->position(location.slot);
    if (position == found->second->rows.size())
        fail(ErrorCode::state, "Index returned a missing row");
    return found->second->rows[position];
}
using RowSelection = std::optional<std::span<const RowLocation>>;
// Sorted locations preserve scan order and let us fetch each selected chunk once.
// Validate provider output before dereferencing it, even for exact results.
template <class F>
void visit_chunks(const detail::Table& table, const std::optional<IndexResult>& selected, F&& visit) {
    if (!selected && !table.selection) {
        for (const auto& [id, chunk] : table.chunks) {
#ifdef CORESQL_TESTING
            ++detail::visited_chunks();
#endif
            if (!visit(id, chunk, RowSelection{}))
                break;
        }
    } else {
        auto rows = selected ? std::span<const RowLocation>(selected->rows)
                             : std::span<const RowLocation>(*table.selection);
        std::vector<RowLocation> intersection;
        if (selected && table.selection) {
            std::set_intersection(rows.begin(), rows.end(), table.selection->begin(), table.selection->end(),
                                  std::back_inserter(intersection));
            rows = intersection;
        }
        for (std::size_t first = 0; first < rows.size();) {
            auto last = first + 1;
            while (last < rows.size() && rows[last].chunk == rows[first].chunk)
                ++last;
            auto chunk = table.chunks.find(rows[first].chunk);
            if (chunk == table.chunks.end())
                fail(ErrorCode::state, "Index returned a missing chunk");
            auto group = rows.subspan(first, last - first);
            for (auto location : group)
                if (chunk->second->position(location.slot) == chunk->second->rows.size())
                    fail(ErrorCode::state, "Index returned a missing row");
#ifdef CORESQL_TESTING
            ++detail::visited_chunks();
#endif
            if (!visit(chunk->first, chunk->second, RowSelection{group}))
                break;
            first = last;
        }
    }
}
template <class F> bool visit_rows(const detail::Chunk& chunk, RowSelection selected, F&& visit) {
    if (selected) {
        for (auto location : *selected) {
            const auto i = chunk.position(location.slot);
            if (!visit(i, chunk.rows[i]))
                return false;
        }
    } else {
        for (std::size_t i = 0; i < chunk.rows.size(); ++i)
            if (!visit(i, chunk.rows[i]))
                return false;
    }
    return true;
}

// Every query reader must honor a private table's row selection.
template <class F> bool visit_table_rows(const detail::Table& table, F&& visit) {
    bool more = true;
    visit_chunks(table, {}, [&](auto id, const auto& chunk, RowSelection rows) {
        more = visit_rows(*chunk, rows, [&](auto position, const Row& row) {
            return visit(RowLocation{id, chunk->slot(position)}, row, chunk->rowids[position]);
        });
        return more;
    });
    return more;
}

} // namespace coresql::detail::execution
