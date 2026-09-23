#include "bound.hpp"
#include "scan.hpp"
#include "index.hpp"
#include "storage.hpp"
#include "stored_value.hpp"

namespace coresql {
using namespace detail::execution;
using detail::require_table;
namespace {
std::optional<std::size_t> integer_primary_column(const detail::Table& table) {
    if (!table.primary)
        return {};
    const auto column = table.primary->column;
    if (column >= table.columns.size() || table.columns[column].type != integer())
        return {};
    return column;
}
void note_inserted_integer_key(detail::Table& table, std::int64_t key) {
    if (!table.integer_pk_known) {
        // The first row is the maximum. A later row must not invent one while
        // an older key may still be larger. row_count already includes this row.
        if (table.row_count == 1) {
            table.integer_pk_known = true;
            table.integer_pk_max = key;
        }
        return;
    }
    if (!table.integer_pk_max || key > *table.integer_pk_max)
        table.integer_pk_max = key;
}
void forget_integer_key(detail::Table& table, const Value& value) {
    if (!table.integer_pk_known)
        return;
    const auto* key = std::get_if<std::int64_t>(&value);
    if (!key || (table.integer_pk_max && *key == *table.integer_pk_max)) {
        table.integer_pk_known = false;
        table.integer_pk_max.reset();
    }
}
void observe_integer_key_change(detail::Table& table, const Value& previous, const Value& next) {
    const auto* old_key = std::get_if<std::int64_t>(&previous);
    const auto* new_key = std::get_if<std::int64_t>(&next);
    if (old_key && new_key && *old_key == *new_key)
        return;
    if (!table.integer_pk_known || !new_key) {
        table.integer_pk_known = false;
        table.integer_pk_max.reset();
        return;
    }
    const bool removed_maximum = old_key && table.integer_pk_max && *old_key == *table.integer_pk_max;
    if (!table.integer_pk_max || *new_key > *table.integer_pk_max)
        table.integer_pk_max = *new_key;
    if (removed_maximum && *new_key != *table.integer_pk_max) {
        table.integer_pk_known = false;
        table.integer_pk_max.reset();
    }
}
} // namespace

void Transaction::insert(const std::string& name, Row row) {
    insert_impl(name, std::move(row), {});
}
void Transaction::insert_impl(const std::string& name, Row row, std::optional<std::int64_t> identity) {
    auto owner = active();
    if (identity && (*identity <= 0 || *identity == INT64_MAX))
        fail(ErrorCode::format, "Invalid row identity");
    auto& stored = require_table(staged_->tables, name);
    detail::prepare_row(row, stored->columns, owner->registry);
    detail::validate_insert_constraints(staged_->tables, name, row, owner->registry);
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
        table.chunks.empty() || table.chunks.rbegin()->second.rows() >= detail::chunk_rows ||
        table.chunks.rbegin()->second.pin()->next_slot() > std::numeric_limits<std::uint32_t>::max() ||
        table.chunks.rbegin()->second.encoded_bytes() + added.encoded_bytes > detail::chunk_bytes;
    if (new_chunk && table.next_chunk == std::numeric_limits<std::uint64_t>::max())
        fail(ErrorCode::state, "Chunk identifier space exhausted");
    // Finish every potentially throwing row allocation before touching the
    // primary index. Its strong insert guarantee makes final publication no-throw.
    const auto id = new_chunk ? table.next_chunk : table.chunks.rbegin()->first;
    std::shared_ptr<detail::Chunk> prepared;
    if (new_chunk) {
        table.chunks.reserve_insert(id);
        prepared = std::make_shared<detail::Chunk>();
        prepared->rows.reserve(1);
    } else {
        auto& chunk = table.chunks[id];
        prepared = chunk.writable();
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
    std::optional<std::int64_t> inserted_key;
    if (auto column = integer_primary_column(table)) {
        if (const auto* key = std::get_if<std::int64_t>(&added.rows.front()[*column]))
            inserted_key = *key;
        else {
            table.integer_pk_known = false;
            table.integer_pk_max.reset();
        }
    }
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
    if (inserted_key)
        note_inserted_integer_key(table, *inserted_key);
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
    return update_impl(name, std::move(assignments), std::move(where), nullptr);
}
Result Transaction::update_returning(const std::string& name, std::vector<Assignment> assignments,
                                     std::optional<Predicate> where) {
    active();
    Result result;
    for (const auto& column : require_table(staged_->tables, name)->columns)
        result.types.push_back(column.type);
    update_impl(name, std::move(assignments), std::move(where), &result.rows);
    return result;
}
Result Transaction::erase_returning(const std::string& name, std::optional<Predicate> where) {
    active();
    Result result;
    for (const auto& column : require_table(staged_->tables, name)->columns)
        result.types.push_back(column.type);
    erase_impl(name, std::move(where), {}, &result.rows);
    return result;
}
std::size_t Transaction::update_impl(const std::string& name, std::vector<Assignment> assignments,
                                     std::optional<Predicate> where, std::vector<Row>* returning) {
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
    auto selected = candidates(original, predicate, owner->registry);
    if (selected)
        normalize(*selected);
    // A point update that leaves keys and relational constraints untouched can
    // prepare just its new row. Publication then needs no throwing callbacks;
    // writable() still copies any chunk retained by a snapshot or savepoint.
    auto row_only = [&] {
        if (!selected || selected->rows.size() != 1 || !original.constraints.checks.empty() ||
            !original.constraints.foreign_keys.empty() ||
            (original.primary && seen.contains(original.primary->column)))
            return false;
        for (const auto& index : original.indexes)
            if (seen.contains(index->column))
                return false;
        for (const auto& index : original.ordered)
            for (auto column : index->columns)
                if (seen.contains(column))
                    return false;
        for (const auto& [other, table] : staged_->tables)
            for (const auto& key : table->constraints.foreign_keys)
                if (key.referenced_table == name)
                    return false;
        return true;
    };
    if (row_only()) {
        std::optional<Row> prepared;
        bool matched = false;
        std::size_t position = 0;
        visit_chunks(original, selected, [&](auto, const auto& chunk, RowSelection rows) {
            visit_rows(*chunk, rows, [&](auto i, const Row& row) {
                if (predicate && !predicate->matches(row, owner->registry))
                    return true;
                matched = true;
                position = i;
                for (const auto& [index, expression] : bound) {
                    auto value = expression.evaluate(row, owner->registry);
                    detail::validate_stored(value, original.columns[index], owner->registry);
                    if (!prepared && !detail::same_stored_value(row[index], value))
                        prepared = row;
                    if (prepared)
                        (*prepared)[index] = std::move(value);
                }
                if (returning)
                    returning->push_back(prepared ? *prepared : row);
                return true;
            });
            return true;
        });
        if (!prepared)
            return matched ? 1 : 0;
        auto replacement = stored.use_count() == 1 ? stored : std::make_shared<detail::Table>(original);
        auto& chunk = replacement->chunks[selected->rows.front().chunk].writable();
        const auto prior_bytes = chunk->payload_bytes;
        chunk->rows[position].swap(*prepared);
        chunk->encoding_id = detail::next_chunk_encoding_id();
        detail::refresh(*chunk);
        replacement->payload_bytes = replacement->payload_bytes - prior_bytes + chunk->payload_bytes;
        stored = std::move(replacement);
        dirty_ = true;
        return 1;
    }
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
                    if (!replacement)
                        replacement = std::make_shared<detail::Table>(original);
                    if (auto column = integer_primary_column(*replacement))
                        if (index == *column)
                            observe_integer_key_change(*replacement, edited->rows[i][index], value);
                    edited->rows[i][index] = std::move(value);
                }
                if (returning)
                    returning->push_back(edited->rows[i]);
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
        detail::validate_replacement_constraints(staged_->tables, name, replacement, owner->registry);
        stored = std::move(replacement);
        dirty_ = true;
    }
    return changed;
}
std::size_t Transaction::erase(const std::string& name, std::optional<Predicate> where) {
    return erase_impl(name, std::move(where), {});
}
std::size_t Transaction::erase_impl(const std::string& name, std::optional<Predicate> where,
                                    std::optional<IndexResult> forced, std::vector<Row>* returning) {
    auto owner = active();
    auto& stored = require_table(staged_->tables, name);
    const auto& original = *stored;
    if (!where && !forced && !returning) {
        const auto count = original.row_count;
        if (!count)
            return 0;
        // Publish empty structures without visiting keys or copying row metadata.
        // Keep the ID sequence monotonic for recovery and existing snapshots.
        auto empty = std::make_shared<detail::Table>();
        empty->columns = original.columns;
        empty->constraints = original.constraints;
        empty->index_definitions = original.index_definitions;
        empty->next_chunk = original.next_chunk;
        empty->next_rowid = original.next_rowid;
        if (integer_primary_column(original))
            empty->integer_pk_known = true;
        detail::make_indexes(*empty, owner->registry);
        detail::validate_replacement_constraints(staged_->tables, name, empty, owner->registry);
        stored = std::move(empty);
        dirty_ = true;
        return count;
    }
    const bool indexed = original.primary || !original.indexes.empty() || !original.ordered.empty();
    auto predicate = bind_predicate(where, original, owner->registry);
    std::shared_ptr<detail::Table> replacement;
    std::size_t changed = 0;
    auto selected = forced ? std::move(forced) : candidates(original, predicate, owner->registry);
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
                if (returning)
                    returning->push_back(row);
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
                    if (auto column = integer_primary_column(*replacement))
                        forget_integer_key(*replacement, row[*column]);
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
        if (replacement->row_count == 0 && integer_primary_column(*replacement)) {
            replacement->integer_pk_known = true;
            replacement->integer_pk_max.reset();
        }
        detail::validate_replacement_constraints(staged_->tables, name, replacement, owner->registry);
        stored = std::move(replacement);
        dirty_ = true;
    }
    return changed;
}
std::optional<std::int64_t> Transaction::maximum_integer_key(const std::string& name) {
    active();
    auto& stored = require_table(staged_->tables, name);
    if (!integer_primary_column(*stored))
        return {};
    if (!stored->integer_pk_known) {
        const auto column = *integer_primary_column(*stored);
        std::optional<std::int64_t> maximum;
        visit_table_rows(*stored, [&](auto, const Row& row, auto) {
            if (const auto* key = std::get_if<std::int64_t>(&row[column]))
                if (!maximum || *key > *maximum)
                    maximum = *key;
            return true;
        });
        // Remember the derived maximum on this transaction's table shell.
        // Publishing it later is a consequence of a real mutation, not a record.
        if (stored.use_count() != 1)
            stored = std::make_shared<detail::Table>(*stored);
        stored->integer_pk_known = true;
        stored->integer_pk_max = maximum;
    }
    return stored->integer_pk_max;
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
    if (conflicts.rows.empty()) {
        insert(name, std::move(row));
        return;
    }
    auto point = savepoint();
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
