#include "bound.hpp"
#include <set>

namespace coresql::detail::execution {
std::string materialize(detail::Tables& tables, Result data) {
#ifdef CORESQL_TESTING
    detail::record_query_stage("materialize", data);
    if (auto* counters = detail::active_query_counters) {
        counters->intermediate_rows += data.rows.size();
        counters->largest_intermediate = std::max(counters->largest_intermediate, data.rows.size());
    }
#endif
    std::string name = "\x01result";
    while (tables.contains(name))
        name += '_';
    auto table = std::make_shared<detail::Table>();
    for (const auto& type : data.types)
        table->columns.push_back({std::to_string(table->columns.size()), type});
    table->row_count = data.rows.size();
    for (auto& row : data.rows) {
        if (table->chunks.empty() || table->chunks.rbegin()->second.rows() == detail::chunk_rows)
            table->chunks.emplace(table->next_chunk++, std::make_shared<detail::Chunk>());
        auto& chunk = *table->chunks.rbegin()->second.writable();
        chunk.rowids.push_back(table->next_rowid++);
        chunk.rows.push_back(std::move(row));
    }
    tables.emplace(name, std::move(table));
    return name;
}
namespace {
std::set<std::string> references(const Query& q, unsigned depth = 0) {
    if (depth >= 64)
        fail(ErrorCode::schema, "Relation nesting exceeds 64");
    std::set<std::string> result;
    auto merge = [&](const Query& nested) {
        auto names = references(nested, depth + 1);
        result.insert(names.begin(), names.end());
    };
    if (!q.table.empty())
        result.insert(q.table);
    if (q.join)
        result.insert(q.join->table);
    for (const auto& j : q.joins)
        result.insert(j.table);
    for (const auto& [op, part] : q.compounds)
        if (part)
            merge(*part);
    auto copy = q;
    copy.relations.clear();
    copy.compounds.clear();
    transform_query(copy, [&](Expr& e) {
        if (e.subquery)
            merge(*e.subquery);
    });
    std::set<std::string> visited;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& r : q.relations)
            if (result.contains(r.name) && visited.insert(r.name).second) {
                if (!r.query)
                    fail(ErrorCode::schema, "Missing relation query");
                merge(*r.query);
                changed = true;
            }
    }
    for (const auto& r : q.relations)
        result.erase(r.name);
    return result;
}
} // namespace
Result run_relations(const Tables& tables, const Query& query, const Registry& registry) {
    auto working = tables;
    auto next = query;
    next.relations.clear();
    std::map<std::string, const Relation*> definitions;
    for (const auto& r : query.relations)
        if (r.name.empty() || !r.query || !definitions.emplace(r.name, &r).second)
            fail(ErrorCode::schema, "Invalid or duplicate relation definition");
    std::set<std::string> active, complete;
    std::function<void(const std::string&)> load = [&](const std::string& name) {
        if (!definitions.contains(name) || complete.contains(name))
            return;
        if (!active.insert(name).second)
            fail(ErrorCode::unsupported, "Recursive relations are unsupported");
        const auto& relation = *definitions.at(name);
        auto dependencies = references(*relation.query);
        for (const auto& definition : query.relations)
            if (dependencies.contains(definition.name))
                load(definition.name);
        auto source = *relation.query;
        if (!query.limit)
            source.limit = 0;
        auto data = run(working, source, registry);
        if (data.types.size() != relation.columns.size())
            fail(ErrorCode::schema, "Relation output width differs");
        std::set<std::string> names;
        for (std::size_t i = 0; i < data.types.size(); ++i) {
            const auto& c = relation.columns[i];
            if (c.name.empty() || !names.insert(c.name).second || c.type != data.types[i])
                fail(ErrorCode::schema, "Relation output name or type differs");
        }
        auto temporary = materialize(working, std::move(data));
        auto table = working.at(temporary);
        table->columns.clear();
        for (const auto& c : relation.columns)
            table->columns.push_back({c.name, c.type, false, {}, {}, true});
        working.erase(temporary);
        working[name] = std::move(table);
        active.erase(name);
        complete.insert(name);
    };
    auto needed = references(next);
    for (const auto& relation : query.relations)
        if (needed.contains(relation.name))
            load(relation.name);
    return run(working, next, registry);
}
} // namespace coresql::detail::execution
