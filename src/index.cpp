#include "index.hpp"
#include <cassert>
#include <algorithm>
#include <bit>

namespace coresql::detail {
Index& writable_index(std::shared_ptr<IndexBinding>& index) {
    if (index.use_count() != 1) index = std::make_shared<IndexBinding>(*index);
    if (index->data.use_count() != 1) {
        auto cloned = index->data->clone();
        if (!cloned) throw Error(ErrorCode::type, "Index clone returned null");
        index->data = std::move(cloned);
    }
    return *index->data;
}
std::shared_ptr<IndexBinding> make_index(const std::vector<Column>& columns, const Registry& registry) {
    std::shared_ptr<IndexBinding> result;
    for (std::size_t i = 0; i < columns.size(); ++i) if (columns[i].primary_key) {
        if (result) throw Error(ErrorCode::schema, "Only one primary-key column is supported");
        result = std::make_shared<IndexBinding>(IndexBinding{i, registry.index(columns[i].index.empty() ? "core.hash" : columns[i].index, columns[i].type, true)});
    }
    return result;
}
void make_indexes(Table& table, const Registry& registry) {
    for(const auto& c:table.columns) {
        if(c.nullable&&c.primary_key)throw Error(ErrorCode::schema,"Primary keys must be non-nullable");
        if(c.nullable&&!c.index.empty())throw Error(ErrorCode::unsupported,"Nullable inline extension indexes are not yet supported");
    }
    table.ordered.clear();
    std::set<std::string> names;
    for (const auto& d : table.index_definitions) {
        if (!names.insert(d.name).second) throw Error(ErrorCode::schema, "Duplicate index name");
        table.ordered.push_back(std::make_shared<OrderedIndex>(d, table.columns, registry));
    }
    table.primary = make_index(table.columns, registry);
    table.indexes.clear();
    for (std::size_t i = 0; i < table.columns.size(); ++i) if (!table.columns[i].primary_key && !table.columns[i].index.empty())
        table.indexes.push_back(std::make_shared<IndexBinding>(IndexBinding{i, registry.index(table.columns[i].index, table.columns[i].type)}));
}
std::optional<RowLocation> lookup(const Table& table, const Value& value) { return table.primary->data->lookup(value); }
void synchronize_index(const Table& original, Table& replacement, const Registry& registry, const std::set<std::size_t>& assigned) {
    assert(original.chunks.size() == replacement.chunks.size());
    auto synchronize = [&](std::shared_ptr<IndexBinding>& index, bool unique) {
        const auto column = index->column;
        if (!assigned.contains(column)) return;
        const auto& type = original.columns[column].type;
        const auto& addon = registry.addon(type);
        for (bool inserting : {false, true}) for (const auto& [id, chunk] : replacement.chunks) {
            const auto& before = original.chunks.find(id)->second;
            if (before == chunk) continue;
            assert(before->rows.size() == chunk->rows.size());
            for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
                const auto& a = before->rows[i][column]; const auto& b = chunk->rows[i][column];
                // Secondary predicates may distinguish encodings that type equality
                // treats as equal. Only unique keys can use that equivalence shortcut.
                bool same = false;
                if (unique && addon.equal) same = addon.equal(type.parameters, a, b);
                else if (const auto* v = std::get_if<Opaque>(&a)) {
                    auto x = v->bytes(), y = std::get<Opaque>(b).bytes();
                    same = x.size() == y.size() && (x.data() == y.data() || std::equal(x.begin(), x.end(), y.begin()));
                } else if (const auto* v = std::get_if<Compact>(&a)) same = *v == std::get<Compact>(b);
                else if (const auto* v = std::get_if<std::int64_t>(&a)) same = *v == std::get<std::int64_t>(b);
                else if (const auto* v = std::get_if<double>(&a)) same = std::bit_cast<std::uint64_t>(*v) == std::bit_cast<std::uint64_t>(std::get<double>(b));
                else same = std::get<std::string>(a) == std::get<std::string>(b);
                if (same) continue;
                if (inserting) writable_index(index).insert(b, {id, chunk->slot(i)});
                else writable_index(index).erase(a, {id, chunk->slot(i)});
            }
        }
    };
    for (auto& index : replacement.ordered) {
        if (std::none_of(index->columns.begin(), index->columns.end(), [&](auto c) { return assigned.contains(c); })) continue;
        index = std::make_shared<OrderedIndex>(*index);
        for (bool inserting : {false, true}) for (const auto& [id, chunk] : replacement.chunks) {
            const auto& before = original.chunks.find(id)->second;
            if (before == chunk) continue;
            for (std::size_t i=0;i<chunk->rows.size();++i) {
                if (inserting) index->insert(chunk->rows[i], {id,chunk->slot(i)});
                else index->erase(before->rows[i], {id,before->slot(i)});
            }
        }
    }
    if (replacement.primary) synchronize(replacement.primary, true);
    for (auto& index : replacement.indexes) synchronize(index, false);
}
void rebuild_indexes(State& state, const Registry& registry) {
    for (auto& [name, table] : state.tables) {
        (void)name;
        make_indexes(*table, registry);
        for (const auto& [id, chunk] : table->chunks) for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
            const auto& row = chunk->rows[i];
            const RowLocation location{id, chunk->slot(i)};
            for (auto& index : table->ordered) index->insert(row, location);
            if (table->primary) table->primary->data->insert(row[table->primary->column], location);
            for (auto& index : table->indexes) index->data->insert(row[index->column], location);
        }
    }
}
}
