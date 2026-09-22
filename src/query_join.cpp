#include "bound.hpp"
#include "query_stages.hpp"
#include "native_join.hpp"
#include "input_filter.hpp"
#include <set>

namespace coresql::detail::execution {
// Right-side filtering completes before ON/WHERE, even with an empty left side.
// Repeatable queries retain immutable chunks and row locations instead of values.
Result run_filtered_join(const Tables& tables, const Query& query, const Registry& registry) {
    if (query.limit) {
        auto probe = query;
        probe.limit = 0;
        run(tables, probe, registry);
    }
    Query source{query.join->table, {}, query.join->right_where, {}};
    if (!query.limit)
        source.limit = 0;
    auto working = tables;
    auto next = query;
    const auto& original = *require_table(tables, query.join->table);
    if (query.repeatable && query.limit) {
        auto predicate = bind_predicate(source.where, original, registry);
        auto candidates_ = candidates(original, predicate, registry);
        if (candidates_)
            normalize(*candidates_);
        std::vector<RowLocation> selected;
        QueryBuffer selection_memory;
        visit_chunks(original, candidates_, [&](auto id, const auto& chunk, RowSelection rows) {
            return visit_rows(*chunk, rows, [&](auto position, const Row& row) {
#ifdef CORESQL_TESTING
                if (auto* counters = detail::active_query_counters)
                    ++counters->rows_tested;
#endif
                if (predicate->matches(RowView(row, chunk->rowids[position]), registry)) {
                    selection_memory.add(sizeof(RowLocation));
                    selected.push_back({id, chunk->slot(position)});
                }
                return true;
            });
        });
        std::string name = "\x01filtered";
        while (working.contains(name))
            name += '_';
        working.emplace(name, table_subset(original, selected));
        next.join->table = std::move(name);
    } else {
        next.join->table = materialize(working, run(tables, source, registry));
        auto& columns = working.at(next.join->table)->columns;
        for (std::size_t i = 0; i < columns.size(); ++i)
            columns[i].name = original.columns[i].name;
    }
    next.join->right_where.reset();
    return run(working, next, registry);
}

// Materialized relational stages use the ordinary binder/filter/sorter. Temporary
// tables are local to this execution and never enter persistent database state.
// General/outer joins share one materialization path; equality-only inner joins
// retain the indexed executor below. ON matching is independent of final WHERE.
Result run_general_join(const detail::Tables& tables, const Query& query, const Registry& registry) {
    const auto& join = *query.join;
    if (query.alias.empty() || join.alias.empty() || query.alias == join.alias)
        fail(ErrorCode::schema, "Join requires distinct aliases");
    const auto& left = *require_table(tables, query.table);
    const auto& right = *require_table(tables, join.table);
    Scope scope(left);
    scope.left_alias = query.alias;
    scope.right = &right;
    scope.right_alias = join.alias;
    auto condition = join.on;
    if (!condition && !join.cross)
        condition = Predicate{join.left, Compare::equal, join.right};
    auto on = bind_predicate(condition, scope, registry);
    Result data;
    Row null_left, null_right;
    for (const auto& c : left.columns) {
        data.types.push_back(c.type);
        null_left.push_back(Null(c.type));
    }
    for (const auto& c : right.columns) {
        data.types.push_back(c.type);
        null_right.push_back(Null(c.type));
    }
    Query final = query;
    final.join.reset();
    final.alias.clear();
    transform_query(final, [&](Expr& e) {
        if (e.kind == Expr::Kind::row_id)
            fail(ErrorCode::unsupported, "Joined row identity is unsupported");
        if (e.kind == Expr::Kind::column)
            e = column(std::to_string(scope.resolve(e).first));
    });
    auto working = tables;
    final.table = materialize(working, data);
    auto probe = final;
    probe.limit = 0;
    auto shape = run(working, probe, registry);
    if (!query.limit)
        return shape;
    // Preserve the two execution phases: all ON callbacks finish before any
    // WHERE/order/projection callback. Only row ownership changes here.
    const bool borrow = join.kind == JoinKind::inner && !query.distinct && query.group_by.empty() &&
                        !query.having && !query.search &&
                        std::none_of(query.select.begin(), query.select.end(), has_aggregate) &&
                        std::none_of(query.order_by.begin(), query.order_by.end(),
                                     [](const Order& o) { return has_aggregate(o.expression); });
    std::vector<RowView> joined_rows;
    QueryBuffer join_memory;
    std::vector<std::size_t> retained;
    if (!borrow && !query.select.empty()) {
        std::set<std::size_t> needed;
        auto expressions = final;
        transform_query(expressions, [&](Expr& e) {
            if (e.kind == Expr::Kind::column)
                needed.insert(std::stoull(e.name));
        });
        if (needed.empty() && !data.types.empty())
            needed.insert(0); // Carry row multiplicity for COUNT(*) and constants.
        retained.assign(needed.begin(), needed.end());
        std::vector<Type> types;
        for (auto index : retained)
            types.push_back(data.types[index]);
        data.types = std::move(types);
        transform_query(final, [&](Expr& e) {
            if (e.kind == Expr::Kind::column) {
                auto found = std::lower_bound(retained.begin(), retained.end(), std::stoull(e.name));
                e = column(std::to_string(found - retained.begin()));
            }
        });
    } else {
        for (std::size_t i = 0; i < data.types.size(); ++i)
            retained.push_back(i);
    }
    auto right_pins = pin_table(right);
    auto left_pins = borrow ? pin_table(left) : std::vector<std::shared_ptr<Chunk>>{};
    std::vector<const Row*> rights;
    visit_table_rows(right, [&](auto, const Row& row, auto) {
        join_memory.add(2 * sizeof(const Row*) + 1);
        rights.push_back(&row);
        return true;
    });
    NativeJoinPositions lookup;
    std::optional<std::pair<std::size_t, std::size_t>> key_columns;
    const auto* key = on ? &*on : nullptr;
    while (key && key->kind == Predicate::Kind::all && !key->children.empty())
        key = &key->children.front();
    if (key && key->leaf && key->leaf->operation == Compare::equal) {
        const auto& a = key->leaf->left;
        const auto& b = key->leaf->right;
        if (a.kind == Expr::Kind::column && b.kind == Expr::Kind::column && a.type == b.type &&
            native_join_key(registry.addon(a.type))) {
            auto x = a.index, y = b.index;
            if (x >= left.columns.size())
                std::swap(x, y);
            if (x < left.columns.size() && y >= left.columns.size())
                key_columns = {x, y - left.columns.size()};
        }
    }
    std::vector<bool> matched_right(rights.size(), false);
    auto append = [&](const Row& a, const Row& b) {
        if (borrow) {
            join_memory.add(2 * sizeof(RowView));
            joined_rows.emplace_back(a, b);
            return;
        }
        Row row;
        row.reserve(retained.size());
        RowView input(a, b);
        for (auto index : retained)
            row.push_back(input[index]);
        join_memory.add_row(row, sizeof(Row));
        data.rows.push_back(std::move(row));
    };
    visit_table_rows(left, [&](auto, const Row& a, auto) {
        bool matched = false;
        auto candidate = [&](std::size_t i) {
            query_step();
#ifdef CORESQL_TESTING
            if (auto* counters = detail::active_query_counters)
                ++counters->candidate_pairs;
#endif
            if (!on || on->matches(RowView(a, *rights[i]), registry)) {
                matched = true;
                matched_right[i] = true;
                append(a, *rights[i]);
            }
        };
        if (key_columns && !is_null(a[key_columns->first])) {
            for (auto i : lookup.find(rights, key_columns->second, a[key_columns->first], registry,
                                      left.columns[key_columns->first].type))
                candidate(i);
        } else {
            for (std::size_t i = 0; i < rights.size(); ++i)
                candidate(i);
        }
        if (!matched && (join.kind == JoinKind::left || join.kind == JoinKind::full))
            append(a, null_right);
        return true;
    });
    if (join.kind == JoinKind::right || join.kind == JoinKind::full)
        for (std::size_t i = 0; i < rights.size(); ++i)
            if (!matched_right[i])
                append(null_left, *rights[i]);
    if (borrow) {
#ifdef CORESQL_TESTING
        if (auto* counters = detail::active_query_counters)
            counters->stages.push_back({"join_pairs", joined_rows.size(), data.types.size(), 0,
                                        joined_rows.capacity() * sizeof(RowView)});
#endif
        return run_scan(tables, query, registry, {}, nullptr, &joined_rows);
    }
    final.table = materialize(working, std::move(data));
    return run(working, final, registry);
}

Result run_multi_join(const Tables& tables, const Query& query, const Registry& registry) {
    if (query.limit) {
        auto probe = query;
        probe.limit = 0;
        run(tables, probe, registry);
    }
    if (auto prepared = prepare_disjunctive_join(tables, query, registry))
        return run(tables, *prepared, registry);
    if (auto prepared = prepare_membership_join(tables, query, registry))
        return run(tables, *prepared, registry);
    auto check = query;
    transform_query(check, [](Expr& e) {
        if (e.kind == Expr::Kind::row_id)
            fail(ErrorCode::unsupported, "Row identity requires a single table");
    });
    if (query.join || query.alias.empty())
        fail(ErrorCode::schema, "Multi-join requires a base alias and no single join");
    if (auto prepared = prepare_join_inputs(tables, query, registry))
        return run(prepared->first, prepared->second, registry);
    // A two-source chain can use the borrowed-row executor directly. Avoid
    // copying the entire left input before a selective final WHERE or LIMIT.
    if (query.limit && query.repeatable && query.joins.size() == 1 && !query.source_where &&
        !query.select.empty()) {
        const auto& join = query.joins.front();
        if (join.kind == JoinKind::inner && !join.on && !join.right_where) {
            Scope scope(*require_table(tables, query.table));
            scope.left_alias = query.alias;
            scope.right = require_table(tables, join.table).get();
            scope.right_alias = join.alias;
            if (join.cross || (native_join_key(registry.addon(scope.resolve(join.left).second)) &&
                               native_join_key(registry.addon(scope.resolve(join.right).second)))) {
                auto direct = query;
                direct.joins.clear();
                direct.join = join;
                return run(tables, direct, registry);
            }
        }
    }
    const auto& base = *require_table(tables, query.table);
    // Column pruning does not move ON evaluation: retain every ON/capture
    // dependency, including those needed by later outer-join stages.
    const bool trim = query.limit && query.repeatable && !query.select.empty();
    std::set<std::pair<std::string, std::string>> needed;
    if (trim) {
        auto expressions = query;
        transform_query(expressions, [&](Expr& e) {
            if (e.kind == Expr::Kind::column)
                needed.emplace(e.qualifier, e.name);
        });
    }
    auto projected = [&](const std::string& alias, const std::vector<Column>& schema) {
        std::vector<Column> selected;
        if (trim)
            for (const auto& c : schema)
                if (needed.contains({alias, c.name}))
                    selected.push_back(c);
        // Retain a physical row carrier when this source contributes no column.
        return selected.empty() ? schema : selected;
    };
    auto base_columns = projected(query.alias, base.columns);
    Query source{query.table, {}, query.source_where, {}};
    source.alias = query.alias;
    if (trim)
        for (const auto& c : base_columns)
            source.select.push_back(column(query.alias, c.name));
    if (!query.limit)
        source.limit = 0;
    auto prefix = native_join_prefix(tables, query, registry);
    auto source_early =
        query.source_where
            ? std::optional<Predicate>{}
            : available_join_prefix(prefix, [&](const Expr& e) { return e.qualifier == query.alias; });
    auto data =
        source_early ? run_scan(tables, source, registry, source_early) : run(tables, source, registry);
    std::map<std::pair<std::string, std::string>, std::string> mapping;
    std::vector<Column> columns;
    auto append = [&](const std::string& alias, const std::vector<Column>& schema) {
        for (const auto& c : schema) {
            auto name = std::to_string(columns.size());
            if (!mapping.emplace(std::pair{alias, c.name}, name).second)
                fail(ErrorCode::schema, "Duplicate join alias");
            columns.push_back({name, c.type});
        }
    };
    append(query.alias, base_columns);
    detail::Tables working = tables;
    std::string temporary;
    auto publish = [&] {
        if (!temporary.empty())
            working.erase(temporary);
        temporary = materialize(working, std::move(data));
        working.at(temporary)->columns = columns;
    };
    auto native_key = [&](const Expr& e) {
        if (e.kind != Expr::Kind::column)
            return false;
        std::string name;
        if (e.qualifier == query.alias)
            name = query.table;
        for (const auto& join : query.joins)
            if (e.qualifier == join.alias)
                name = join.table;
        if (name.empty())
            return false;
        const auto& schema = require_table(tables, name)->columns;
        return std::any_of(schema.begin(), schema.end(), [&](const Column& c) {
            return c.name == e.name && native_join_key(registry.addon(c.type));
        });
    };
    std::size_t stage = 0;
    for (auto join : query.joins) {
        for (const auto& [key, value] : mapping) {
            (void)value;
            if (key.first == join.alias)
                fail(ErrorCode::schema, "Duplicate join alias");
        }
        const bool stream_final = ++stage == query.joins.size() && query.repeatable &&
                                  !query.select.empty() && join.kind == JoinKind::inner && !join.on &&
                                  (join.cross || (native_key(join.left) && native_key(join.right)));
        publish();
        const std::string left_alias = join.alias == "left" ? "__left" : "left";
        auto rewrite = [&](Expr& e) {
            if (e.kind == Expr::Kind::column && e.qualifier != join.alias) {
                auto p = mapping.find({e.qualifier, e.name});
                if (p == mapping.end())
                    fail(ErrorCode::schema, "Unknown joined column");
                e = column(left_alias, p->second);
            }
        };
        transform_expr(join.left, rewrite);
        transform_expr(join.right, rewrite);
        if (join.on)
            transform_pred(*join.on, rewrite);
        // Let the final scan apply WHERE before copying rows. Do not build the
        // last Cartesian/equality intermediate merely to scan it again.
        if (stream_final) {
            auto final = query;
            final.table = temporary;
            final.alias = left_alias;
            final.joins.clear();
            final.source_where.reset();
            transform_query(final, rewrite);
            final.join = join;
            return run(working, final, registry);
        }
        auto right_columns = projected(join.alias, require_table(tables, join.table)->columns);
        Query step{temporary, {}, {}, {}};
        if (trim) {
            for (const auto& c : columns)
                step.select.push_back(column(left_alias, c.name));
            for (const auto& c : right_columns)
                step.select.push_back(column(join.alias, c.name));
        }
        step.repeatable = query.repeatable;
        step.alias = left_alias;
        step.join = join;
        if (!query.limit)
            step.limit = 0;
        auto early = available_join_prefix(prefix, [&](const Expr& e) {
            return e.qualifier == join.alias || mapping.contains({e.qualifier, e.name});
        });
        if (early)
            transform_pred(*early, rewrite);
        data = early ? run_scan(working, step, registry, early) : run(working, step, registry);
        append(join.alias, right_columns);
    }
    publish();
    Query final = query;
    final.table = temporary;
    final.alias.clear();
    final.joins.clear();
    final.source_where.reset();
    transform_query(final, [&](Expr& e) {
        if (e.kind == Expr::Kind::column) {
            auto p = mapping.find({e.qualifier, e.name});
            if (p == mapping.end())
                fail(ErrorCode::schema, "Unknown joined column");
            e = column(p->second);
        }
    });
    return run(working, final, registry);
}

} // namespace coresql::detail::execution
