#pragma once
#include "state.hpp"
#include "ordered_index.hpp"
#include <set>

namespace coresql::detail {
// Locations are valid only in the associated table snapshot.
struct IndexBinding { std::size_t column; std::shared_ptr<Index> data; };
std::shared_ptr<IndexBinding> make_index(const std::vector<Column>&, const Registry&);
void make_indexes(Table&, const Registry&);
Index& writable_index(std::shared_ptr<IndexBinding>&);
std::optional<RowLocation> lookup(const Table&, const Value&);
// Update-only: chunk IDs and row positions must be preserved.
void synchronize_index(const Table& original, Table& replacement, const Registry&, const std::set<std::size_t>& assigned);
void rebuild_indexes(State&, const Registry&);
}
