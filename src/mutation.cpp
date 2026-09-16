#include "bound.hpp"
#include "scan.hpp"
#include "index.hpp"
#include "storage.hpp"

namespace coresql {
using namespace detail::execution;
using detail::require_table;

void Transaction::insert(const std::string& name, Row row) {
    insert_impl(name, std::move(row), {});
}
void Transaction::insert_impl(const std::string& name, Row row, std::optional<std::int64_t> identity) {
    auto owner = active();
    if (identity && (*identity <= 0 || *identity == INT64_MAX))
        fail(ErrorCode::format, "Invalid row identity");
    auto& stored = require_table(staged_->tables, name);
    detail::prepare_row(row, stored->columns, owner->registry);
    if (stored->primary && detail::lookup(*stored, row[stored->primary->column]))
        fail(ErrorCode::constraint, "Duplicate primary key");
    if (stored.use_count() != 1)
        stored = std::make_shared<detail::Table>(*stored);
    auto& table = *stored;
    detail::Chunk added;
    added.rows.push_back(std::move(row));
    detail::refresh(added);
    const auto bytes = added.payload_bytes;
    const bool new_chunk =
        table.chunks.empty() || table.chunks.rbegin()->second->rows.size() >= detail::chunk_rows ||
        table.chunks.rbegin()->second->next_slot() > std::numeric_limits<std::uint32_t>::max() ||
        table.chunks.rbegin()->second->encoded_bytes + added.encoded_bytes > detail::chunk_bytes;
    if (new_chunk && table.next_chunk == std::numeric_limits<std::uint64_t>::max())
        fail(ErrorCode::state, "Chunk identifier space exhausted");
    // Finish every potentially throwing row allocation before touching the
    // primary index. Its strong insert guarantee makes final publication no-throw.
    const auto id = new_chunk ? table.next_chunk : table.chunks.rbegin()->first;
    std::shared_ptr<detail::Chunk> prepared;
    if (new_chunk) {
        table.chunks.reserve_insert();
        prepared = std::make_shared<detail::Chunk>();
        prepared->rows.reserve(1);
    } else {
        auto& chunk = table.chunks.rbegin()->second;
        if (chunk.use_count() != 1)
            chunk = std::make_shared<detail::Chunk>(*chunk);
        prepared = chunk;
        if (prepared->rows.size() == prepared->rows.capacity())
            prepared->rows.reserve(std::max(std::size_t{1}, prepared->rows.size() * 2));
    }
    // Independent secondary indexes are edited privately; a later failure
    // discards them all. The common primary-only path needs no per-row clone.
    if (!identity && table.next_rowid == INT64_MAX)
        fail(ErrorCode::state, "Row identity exhausted");
    prepared->rowids.reserve(prepared->rows.capacity());
    const RowLocation location{id, static_cast<std::uint32_t>(prepared->next_slot())};
    if (!prepared->slots.empty())
        prepared->slots.reserve(prepared->rows.capacity());
    auto ordered = table.ordered;
    for (auto& index : ordered) {
        index = std::make_shared<detail::OrderedIndex>(*index);
        index->insert(added.rows.front(), location);
    }
    auto indexes = table.indexes;
    for (auto& index : indexes)
        detail::writable_index(index).insert(added.rows.front()[index->column], location);
    if (table.primary)
        detail::writable_index(table.primary).insert(added.rows.front()[table.primary->column], location);
    if (!prepared->slots.empty())
        prepared->slots.push_back(location.slot);
    const auto rowid = identity.value_or(table.next_rowid);
    prepared->rowids.push_back(rowid);
    table.next_rowid = std::max(table.next_rowid, rowid + 1);
    prepared->rows.push_back(std::move(added.rows.front()));
    prepared->payload_bytes += bytes;
    prepared->encoded_bytes += added.encoded_bytes;
    if (new_chunk) {
        table.chunks.emplace(id, std::move(prepared));
        ++table.next_chunk;
    }
    table.indexes = std::move(indexes);
    table.ordered = std::move(ordered);
    ++table.row_count;
    table.payload_bytes += bytes;
    dirty_ = true;
}
void Transaction::restore_row_sequence(const std::string& name, std::int64_t next) {
    active();
    auto& stored = require_table(staged_->tables, name);
    if (next < stored->next_rowid)
        fail(ErrorCode::format, "Invalid row identity sequence");
    if (stored.use_count() != 1)
        stored = std::make_shared<detail::Table>(*stored);
    stored->next_rowid = next;
    dirty_ = true;
}
std::size_t Transaction::update(const std::string& name, std::vector<Assignment> assignments,
                                std::optional<Predicate> where) {
    auto owner = active();
    auto& stored = require_table(staged_->tables, name);
    const auto& original = *stored;
    if (assignments.empty())
        fail(ErrorCode::schema, "Update requires assignments");
    std::set<std::size_t> seen;
    std::vector<std::pair<std::size_t, BoundExpr>> bound;
    for (const auto& assignment : assignments) {
        auto target = bind(column(assignment.column), original, owner->registry);
        if (!seen.insert(target.index).second)
            fail(ErrorCode::schema, "Duplicate assignment: " + assignment.column);
        auto value = bind(assignment.value, original, owner->registry);
        if (value.type != target.type)
            fail(ErrorCode::type, "Assignment type differs from column");
        bound.emplace_back(target.index, std::move(value));
    }
    auto predicate = bind_predicate(where, original, owner->registry);
    auto selected = candidates(original, predicate);
    if (selected)
        normalize(*selected);
    const auto* comparison = predicate ? predicate->direct_comparison() : nullptr;
    std::shared_ptr<detail::Table> replacement;
    std::size_t changed = 0;
    auto apply = [&](auto&& matches) {
        visit_chunks(original, selected, [&](auto id, const auto& chunk, RowSelection rows) {
            std::shared_ptr<detail::Chunk> edited;
            visit_rows(*chunk, rows, [&](auto i, const Row& row) {
                if (!matches(row))
                    return true;
                if (!edited)
                    edited = std::make_shared<detail::Chunk>(*chunk);
                for (const auto& [index, expression] : bound) {
                    auto value = expression.evaluate(row, owner->registry);
                    detail::validate_stored(value, original.columns[index], owner->registry);
                    edited->rows[i][index] = std::move(value);
                }
                ++changed;
                return true;
            });
            if (edited) {
                if (!replacement)
                    replacement = std::make_shared<detail::Table>(original);
                detail::refresh(*edited);
                replacement->chunks[id] = std::move(edited);
            }
            return true;
        });
    };
    // Bind this common shape once, rather than constructing expression temporaries
    // and dispatching a recursive predicate for every scanned row. Still uses the
    // registered type comparison and preserves every error/ordering rule.
    if (comparison) {
        const auto index = comparison->left.index;
        const auto& value = comparison->right.value;
        apply([&](const Row& row) { return comparison->values(row[index], value); });
    } else
        apply([&](const Row& row) { return !predicate || predicate->matches(row, owner->registry); });
    if (changed) {
        if ((original.primary && seen.contains(original.primary->column)) || !original.indexes.empty() ||
            !original.ordered.empty())
            detail::synchronize_index(original, *replacement, owner->registry, seen);
        detail::refresh(*replacement);
        stored = std::move(replacement);
        dirty_ = true;
    }
    return changed;
}
std::size_t Transaction::erase(const std::string& name, std::optional<Predicate> where) {
    return erase_impl(name, std::move(where), {});
}
std::size_t Transaction::erase_impl(const std::string& name, std::optional<Predicate> where,
                                    std::optional<IndexResult> forced) {
    auto owner = active();
    auto& stored = require_table(staged_->tables, name);
    const auto& original = *stored;
    if (!where && !forced) {
        const auto count = original.row_count;
        if (!count)
            return 0;
        // Publish empty structures without visiting keys or copying row metadata.
        // Keep the ID sequence monotonic for recovery and existing snapshots.
        auto empty = std::make_shared<detail::Table>();
        empty->columns = original.columns;
        empty->index_definitions = original.index_definitions;
        empty->next_chunk = original.next_chunk;
        empty->next_rowid = original.next_rowid;
        detail::make_indexes(*empty, owner->registry);
        stored = std::move(empty);
        dirty_ = true;
        return count;
    }
    const bool indexed = original.primary || !original.indexes.empty() || !original.ordered.empty();
    auto predicate = bind_predicate(where, original, owner->registry);
    std::shared_ptr<detail::Table> replacement;
    std::size_t changed = 0;
    auto selected = forced ? std::move(forced) : candidates(original, predicate);
    if (selected)
        normalize(*selected);
    visit_chunks(original, selected, [&](auto id, const auto& chunk, RowSelection rows) {
        // Only allocate/copy rows after the first deletion in this chunk.
        std::shared_ptr<detail::Chunk> edited;
        for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
            const auto& row = chunk->rows[i];
            const RowLocation location{id, chunk->slot(i)};
            const bool eligible = !rows || std::binary_search(rows->begin(), rows->end(), location);
            if (eligible && (!predicate || predicate->matches(row, owner->registry))) {
                if (!replacement)
                    replacement = std::make_shared<detail::Table>(original);
                if (!edited) {
                    edited = std::make_shared<detail::Chunk>();
                    edited->rows.assign(chunk->rows.begin(),
                                        chunk->rows.begin() + static_cast<std::ptrdiff_t>(i));
                    edited->rowids.assign(chunk->rowids.begin(),
                                          chunk->rowids.begin() + static_cast<std::ptrdiff_t>(i));
                    if (indexed) {
                        edited->slots.reserve(chunk->rows.size());
                        for (std::size_t kept = 0; kept < i; ++kept)
                            edited->slots.push_back(chunk->slot(kept));
                    }
                }
                for (auto& index : replacement->ordered) {
                    if (index.use_count() != 1)
                        index = std::make_shared<detail::OrderedIndex>(*index);
                    index->erase(row, location);
                }
                if (original.primary) {
                    detail::writable_index(replacement->primary)
                        .erase(row[original.primary->column], location);
                }
                for (auto& index : replacement->indexes)
                    detail::writable_index(index).erase(row[index->column], location);
                ++changed;
            } else if (edited) {
                if (indexed)
                    edited->slots.push_back(location.slot);
                edited->rows.push_back(row);
                edited->rowids.push_back(chunk->rowids[i]);
            }
        }
        if (edited) {
            if (edited->rows.empty())
                replacement->chunks[id].reset();
            else {
                detail::refresh(*edited);
                replacement->chunks[id] = std::move(edited);
            }
        }
        return true;
    });
    if (changed) {
        replacement->chunks.remove_empty();
        detail::refresh(*replacement);
        stored = std::move(replacement);
        dirty_ = true;
    }
    return changed;
}
void Transaction::replace(const std::string& name, Row row) {
    auto owner = active();
    const auto& table = *require_table(staged_->tables, name);
    detail::prepare_row(row, table.columns, owner->registry);
    IndexResult conflicts{{}, true};
    if (table.primary)
        if (auto p = detail::lookup(table, row[table.primary->column]))
            conflicts.rows.push_back(*p);
    for (const auto& index : table.ordered)
        if (index->definition.unique) {
            auto key = index->key(row);
            if (std::any_of(key.begin(), key.end(), is_null))
                continue;
            auto found = index->equal(key);
            conflicts.rows.insert(conflicts.rows.end(), found.begin(), found.end());
        }
    auto point = savepoint();
    if (!conflicts.rows.empty())
        erase_impl(name, {}, std::move(conflicts));
    insert(name, std::move(row));
    point.release();
}
void Transaction::insert_from(const std::string& name, const Query& query, bool replacing) {
    active();
    const auto& target = *require_table(staged_->tables, name);
    // Read before writing: self-insertion cannot feed on rows it just inserted.
    auto result = this->query(query);
    if (result.types.size() > target.columns.size())
        fail(ErrorCode::schema, "Insertion projection is too wide");
    for (std::size_t i = 0; i < target.columns.size(); ++i) {
        if (i < result.types.size()) {
            if (result.types[i] != target.columns[i].type)
                fail(ErrorCode::type, "Insertion projection type differs");
        } else if (!target.columns[i].default_value)
            fail(ErrorCode::schema, "Missing insertion column default");
    }
    auto point = savepoint();
    for (auto& row : result.rows) {
        if (replacing)
            replace(name, std::move(row));
        else
            insert(name, std::move(row));
    }
    point.release();
}

} // namespace coresql
