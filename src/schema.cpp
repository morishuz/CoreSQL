#include "storage.hpp"
#include "index.hpp"
#include "comparison.hpp"
#include "bound.hpp"
#include <set>

namespace coresql {
namespace {
[[noreturn]] void fail(ErrorCode c, const std::string& s) {
    throw Error(c, s);
}
} // namespace
using detail::require_table;
void detail::validate_stored(const Value& value, const Column& column, const Registry& registry) {
    if (is_null(value) && !column.nullable)
        throw Error(ErrorCode::constraint, "NULL in non-nullable column: " + column.name);
    registry.validate(value, column.type);
}
void detail::prepare_row(Row& row, const std::vector<Column>& columns, const Registry& registry) {
    while (row.size() < columns.size() && columns[row.size()].default_value)
        row.push_back(*columns[row.size()].default_value);
    if (row.size() != columns.size())
        fail(ErrorCode::schema, "Row width differs from schema");
    for (std::size_t i = 0; i < row.size(); ++i)
        validate_stored(row[i], columns[i], registry);
}
void Transaction::create_table(std::string name, std::vector<Column> columns) {
    auto owner = active();
    if (name.empty() || columns.empty())
        fail(ErrorCode::schema, "Table needs a name and columns");
    std::set<std::string> names;
    for (const auto& c : columns) {
        if (c.name.empty() || !names.insert(c.name).second)
            fail(ErrorCode::schema, "Empty or duplicate column name");
        owner->registry.validate(c.type);
        if (c.default_value)
            detail::validate_stored(*c.default_value, c, owner->registry);
    }
    if (staged_->tables.contains(name))
        fail(ErrorCode::schema, "Table already exists");
    auto table = std::make_shared<detail::Table>();
    table->columns = std::move(columns);
    detail::make_indexes(*table, owner->registry);
    staged_->tables.emplace(std::move(name), std::move(table));
    dirty_ = true;
}
void Transaction::vacuum() {
    auto owner = active();
    auto next = std::make_unique<detail::State>(*staged_);
    for (auto& [name, stored] : next->tables) {
        (void)name;
        auto table = std::make_shared<detail::Table>(*stored);
        table->chunks = {};
        for (const auto& [id, reference] : stored->chunks) {
            const auto chunk = reference.pin();
            (void)id;
            for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
                if (table->chunks.empty() || table->chunks.rbegin()->second.rows() >= detail::chunk_rows ||
                    table->chunks.rbegin()->second.encoded_bytes() >= detail::chunk_bytes) {
                    if (table->next_chunk == UINT64_MAX)
                        fail(ErrorCode::state, "Chunk identity exhausted");
                    table->chunks.emplace(table->next_chunk++, std::make_shared<detail::Chunk>());
                }
                auto& out = *table->chunks[table->chunks.rbegin()->first].writable();
                out.rows.push_back(chunk->rows[i]);
                out.rowids.push_back(chunk->rowids[i]);
                detail::refresh(out);
            }
        }
        stored = std::move(table);
    }
    detail::rebuild_indexes(*next, owner->registry);
    staged_ = std::move(next);
    dirty_ = true;
}
void Transaction::integrity_check() const {
    auto owner = active();
    detail::validate_constraints(staged_->tables, owner->registry);
    for (const auto& [name, table] : staged_->tables) {
        (void)name;
        std::set<std::int64_t> ids;
        std::size_t rows = 0, bytes = 0;
        for (const auto& index : table->ordered)
            if (index->validate() != table->row_count)
                fail(ErrorCode::state, "Ordered index cardinality mismatch");
        for (const auto& [id, reference] : table->chunks) {
            const auto chunk = reference.pin();
            if (id >= table->next_chunk || chunk->rows.size() != chunk->rowids.size())
                fail(ErrorCode::state, "Invalid chunk metadata");
            auto measured = *chunk;
            detail::refresh(measured);
            if (measured.payload_bytes != chunk->payload_bytes ||
                measured.encoded_bytes != chunk->encoded_bytes)
                fail(ErrorCode::state, "Invalid byte accounting");
            rows += chunk->rows.size();
            bytes += chunk->payload_bytes;
            for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
                const auto& row = chunk->rows[i];
                if (row.size() != table->columns.size() || chunk->rowids[i] <= 0 ||
                    chunk->rowids[i] >= table->next_rowid || !ids.insert(chunk->rowids[i]).second)
                    fail(ErrorCode::state, "Invalid row metadata");
                for (std::size_t c = 0; c < row.size(); ++c) {
                    detail::validate_stored(row[c], table->columns[c], owner->registry);
                }
                RowLocation location{id, chunk->slot(i)};
                for (const auto& index : table->ordered) {
                    auto found = index->equal(index->key(row));
                    if (std::find(found.begin(), found.end(), location) == found.end())
                        fail(ErrorCode::state, "Ordered index mismatch");
                }
            }
        }
        auto validate_index = [&](const detail::IndexBinding& index) {
            std::vector<IndexEntry> source;
            source.reserve(rows);
            for (const auto& [id, reference] : table->chunks) {
                const auto chunk = reference.pin();
                for (std::size_t i = 0; i < chunk->rows.size(); ++i)
                    source.push_back({chunk->rows[i][index.column], {id, chunk->slot(i)}});
            }
            index.data->validate(source);
        };
        if (table->primary)
            validate_index(*table->primary);
        for (const auto& index : table->indexes)
            validate_index(*index);
        if (rows != table->row_count || bytes != table->payload_bytes)
            fail(ErrorCode::state, "Invalid table accounting");
    }
}
std::map<std::string, std::size_t> Transaction::analyze() const {
    auto owner = active();
    std::map<std::string, std::size_t> result;
    for (const auto& [name, table] : staged_->tables) {
        result[name + ".rows"] = table->row_count;
        for (const auto& index : table->ordered) {
            std::set<Row, detail::RowLess> keys(detail::RowLess{&owner->registry});
            for (const auto& [id, reference] : table->chunks) {
                const auto chunk = reference.pin();
                (void)id;
                for (const auto& row : chunk->rows)
                    keys.insert(index->key(row));
            }
            result[name + "." + index->definition.name + ".distinct"] = keys.size();
        }
    }
    return result;
}
void Transaction::create_index(const std::string& name, IndexDefinition definition) {
    auto owner = active();
    auto& stored = require_table(staged_->tables, name);
    for (const auto& d : stored->index_definitions)
        if (d.name == definition.name)
            fail(ErrorCode::schema, "Index already exists");
    auto replacement = std::make_shared<detail::Table>(*stored);
    auto index = std::make_shared<detail::OrderedIndex>(definition, stored->columns, owner->registry);
    for (const auto& [id, reference] : stored->chunks) {
        const auto chunk = reference.pin();
        for (std::size_t i = 0; i < chunk->rows.size(); ++i)
            index->insert(chunk->rows[i], {id, chunk->slot(i)});
    }
    replacement->index_definitions.push_back(std::move(definition));
    replacement->ordered.push_back(std::move(index));
    stored = std::move(replacement);
    dirty_ = true;
}
void Transaction::add_column(const std::string& name, Column column) {
    auto owner = active();
    auto& stored = require_table(staged_->tables, name);
    if (column.name.empty() || column.primary_key || !column.index.empty() || !column.default_value)
        fail(ErrorCode::schema, "Added column needs a default and no inline index");
    for (const auto& c : stored->columns)
        if (c.name == column.name)
            fail(ErrorCode::schema, "Duplicate column");
    detail::validate_stored(*column.default_value, column, owner->registry);
    auto replacement = std::make_shared<detail::Table>(*stored);
    replacement->columns.push_back(column);
    for (const auto& [id, reference] : stored->chunks) {
        const auto chunk = reference.pin();
        auto edited = std::make_shared<detail::Chunk>(*chunk);
        for (auto& row : edited->rows)
            row.push_back(*column.default_value);
        detail::refresh(*edited);
        replacement->chunks[id] = std::move(edited);
    }
    detail::refresh(*replacement);
    stored = std::move(replacement);
    dirty_ = true;
}
} // namespace coresql

namespace coresql {
std::vector<IndexDefinition> Transaction::indexes(const std::string& name) const {
    active();
    return detail::require_table(staged_->tables, name)->index_definitions;
}
void Transaction::drop_table(const std::string& name) {
    active();
    detail::require_table(staged_->tables, name);
    for (const auto& [other, table] : staged_->tables)
        if (other != name)
            for (const auto& key : table->constraints.foreign_keys)
                if (key.referenced_table == name)
                    throw Error(ErrorCode::constraint, "Cannot drop a referenced table");
    auto epoch = std::make_shared<const int>(0);
    staged_->tables.erase(name);
    staged_->schema_epoch = std::move(epoch);
    dirty_ = true;
}
void Transaction::drop_index(const std::string& name, const std::string& index) {
    auto owner = active();
    auto& stored = detail::require_table(staged_->tables, name);
    auto replacement = std::make_shared<detail::Table>(*stored);
    auto it = std::find_if(replacement->index_definitions.begin(), replacement->index_definitions.end(),
                           [&](const IndexDefinition& d) { return d.name == index; });
    if (it == replacement->index_definitions.end())
        throw Error(ErrorCode::schema, "Unknown index: " + index);
    if (it->constraint_owned)
        throw Error(ErrorCode::constraint, "Cannot drop a constraint-owned index");
    auto position = it - replacement->index_definitions.begin();
    replacement->index_definitions.erase(it);
    replacement->ordered.erase(replacement->ordered.begin() + position);
    detail::validate_replacement_constraints(staged_->tables, name, replacement, owner->registry);
    stored = std::move(replacement);
    dirty_ = true;
}
void Transaction::rename_table(const std::string& name, const std::string& replacement) {
    auto owner = active();
    auto table = detail::require_table(staged_->tables, name);
    if (replacement.empty() || staged_->tables.contains(replacement))
        throw Error(ErrorCode::schema, "Empty or existing destination table");
    auto epoch = std::make_shared<const int>(0);
    auto candidate = staged_->tables;
    candidate.emplace(replacement, std::move(table));
    candidate.erase(name);
    for (auto& [other, stored] : candidate) {
        (void)other;
        bool affected =
            std::any_of(stored->constraints.foreign_keys.begin(), stored->constraints.foreign_keys.end(),
                        [&](const ForeignKey& key) { return key.referenced_table == name; });
        if (affected) {
            stored = std::make_shared<detail::Table>(*stored);
            for (auto& key : stored->constraints.foreign_keys)
                if (key.referenced_table == name)
                    key.referenced_table = replacement;
        }
    }
    detail::validate_constraints(candidate, owner->registry);
    staged_->tables = std::move(candidate);
    staged_->schema_epoch = std::move(epoch);
    dirty_ = true;
}
void Transaction::rename_column(const std::string& name, const std::string& column,
                                const std::string& replacement) {
    auto owner = active();
    auto& stored = detail::require_table(staged_->tables, name);
    auto table = std::make_shared<detail::Table>(*stored);
    if (replacement.empty() || std::any_of(table->columns.begin(), table->columns.end(),
                                           [&](const Column& c) { return c.name == replacement; }))
        throw Error(ErrorCode::schema, "Empty or existing destination column");
    auto it = std::find_if(table->columns.begin(), table->columns.end(),
                           [&](const Column& c) { return c.name == column; });
    if (it == table->columns.end())
        throw Error(ErrorCode::schema, "Unknown column: " + column);
    it->name = replacement;
    for (auto& d : table->index_definitions)
        for (auto& c : d.columns)
            if (c == column)
                c = replacement;
    for (auto& check : table->constraints.checks)
        detail::execution::transform_expr(check.expression, [&](Expr& e) {
            if (e.kind == Expr::Kind::column && e.name == column)
                e.name = replacement;
        });
    for (auto& key : table->constraints.foreign_keys)
        for (auto& c : key.columns)
            if (c == column)
                c = replacement;
    // Rebuild derived indexes against the renamed schema without mutating old snapshots.
    detail::State candidate;
    candidate.tables = staged_->tables;
    candidate.tables[name] = table;
    for (auto& [other, child] : candidate.tables) {
        (void)other;
        bool affected =
            std::any_of(child->constraints.foreign_keys.begin(), child->constraints.foreign_keys.end(),
                        [&](const ForeignKey& key) { return key.referenced_table == name; });
        if (affected) {
            child = std::make_shared<detail::Table>(*child);
            for (auto& key : child->constraints.foreign_keys)
                if (key.referenced_table == name)
                    for (auto& c : key.referenced_columns)
                        if (c == column)
                            c = replacement;
        }
    }
    detail::State rebuilt;
    rebuilt.tables.emplace(name, candidate.tables.at(name));
    detail::rebuild_indexes(rebuilt, owner->registry);
    candidate.tables[name] = rebuilt.tables.at(name);
    auto epoch = std::make_shared<const int>(0);
    detail::validate_constraints(candidate.tables, owner->registry);
    staged_->tables = std::move(candidate.tables);
    staged_->schema_epoch = std::move(epoch);
    dirty_ = true;
}
} // namespace coresql
