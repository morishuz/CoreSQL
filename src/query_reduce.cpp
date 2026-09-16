#include "bound.hpp"
#include "group_table.hpp"
#include "query_stages.hpp"
#include <set>

namespace coresql::detail::execution {
namespace {
struct DistinctAggregate final : AggregateState {
    std::unique_ptr<AggregateState> inner;
    std::set<Row, QueryRowLess> seen;
    DistinctAggregate(std::unique_ptr<AggregateState> state, const Registry& registry,
                      std::span<const Type> types, bool repeatable)
        : inner(std::move(state)), seen(QueryRowLess{registry, types, repeatable}) {}
    void step(std::span<const Value> values) override {
        if (seen.emplace(values.begin(), values.end()).second)
            inner->step(values);
    }
    Value finish() override { return inner->finish(); }
};
} // namespace

Result run_compound(const Tables& tables, const Query& query, const Registry& registry) {
    if (query.limit) {
        auto probe = query;
        probe.limit = 0;
        run(tables, probe, registry);
    }
    auto input = query;
    input.compounds.clear();
    input.order_by.clear();
    input.limit = query.limit ? std::numeric_limits<std::size_t>::max() : 0;
    auto data = run(tables, input, registry);
    for (const auto& [operation, part] : query.compounds) {
        if (!part)
            fail(ErrorCode::schema, "Missing compound query");
        auto arm = *part;
        if (!query.limit)
            arm.limit = 0;
        auto other = run(tables, arm, registry);
        if (data.types != other.types)
            fail(ErrorCode::type, "Compound query column types differ");
        if (operation == SetOperation::union_all) {
            for (auto& row : other.rows)
                data.rows.push_back(std::move(row));
            continue;
        }
        for (const auto& type : data.types)
            if (!registry.orderable(type))
                fail(ErrorCode::unsupported, "Set operation needs orderable values");
        QueryRowLess less{registry, data.types, query.repeatable};
        std::set<Row, decltype(less)> left(data.rows.begin(), data.rows.end(), less),
            right(other.rows.begin(), other.rows.end(), less);
        data.rows.clear();
        if (operation == SetOperation::union_distinct) {
            left.insert(right.begin(), right.end());
            data.rows.assign(left.begin(), left.end());
        } else
            for (const auto& row : left)
                if (right.contains(row) == (operation == SetOperation::intersect))
                    data.rows.push_back(row);
    }
    auto working = tables;
    Query final;
    final.table = materialize(working, std::move(data));
    final.order_by = query.order_by;
    final.limit = query.limit;
    return run(working, final, registry);
}

Result run_grouped(const Tables& tables, const Query& query, const Registry& registry) {
    Query input = query;
    input.select = query.group_by;
    input.group_by.clear();
    input.having.reset();
    input.order_by.clear();
    input.distinct = false;
    input.limit = std::numeric_limits<std::size_t>::max();
    for (const auto& key : query.group_by)
        if (has_aggregate(key))
            fail(ErrorCode::schema, "GROUP BY cannot contain aggregates");
    std::vector<Expr> aggregates;
    Query final;
    final.select = query.select;
    final.where = query.having;
    final.order_by = query.order_by;
    final.limit = query.limit;
    final.distinct = query.distinct;
    if (final.select.empty())
        for (const auto& c : require_table(tables, query.table)->columns)
            final.select.push_back(column(query.alias, c.name));
    std::function<void(Expr&)> rewrite = [&](Expr& e) {
        for (std::size_t i = 0; i < query.group_by.size(); ++i)
            if (same_expression(e, query.group_by[i])) {
                e = column(std::to_string(i));
                return;
            }
        if (e.kind == Expr::Kind::aggregate) {
            for (const auto& arg : e.arguments)
                if (has_aggregate(arg))
                    fail(ErrorCode::schema, "Nested aggregate");
            auto found = std::find_if(aggregates.begin(), aggregates.end(),
                                      [&](const Expr& a) { return same_expression(a, e); });
            auto index = static_cast<std::size_t>(found - aggregates.begin());
            if (found == aggregates.end())
                aggregates.push_back(e);
            e = column(std::to_string(query.group_by.size() + index));
            return;
        }
        if (e.kind == Expr::Kind::column || e.kind == Expr::Kind::row_id)
            fail(ErrorCode::schema, "Column must appear in GROUP BY or an aggregate");
        for (auto& arg : e.arguments)
            rewrite(arg);
    };
    for (auto& e : final.select)
        rewrite(e);
    for (auto& key : final.order_by)
        rewrite(key.expression);
    std::function<void(Predicate&)> rewrite_pred = [&](Predicate& p) {
        rewrite(p.left);
        rewrite(p.right);
        for (auto& child : p.children)
            rewrite_pred(child);
    };
    if (final.where)
        rewrite_pred(*final.where);
    for (const auto& e : aggregates)
        input.select.insert(input.select.end(), e.arguments.begin(), e.arguments.end());
    if (input.select.empty())
        input.select.push_back(literal(std::int64_t{1}));
    auto probe = input;
    probe.limit = 0;
    auto shape = run(tables, probe, registry);
    Result data;
    auto key_types = std::span(shape.types).first(query.group_by.size());
    data.types.assign(key_types.begin(), key_types.end());
    for (const auto& type : data.types)
        if (!registry.orderable(type))
            fail(ErrorCode::unsupported, "GROUP BY needs orderable values");
    std::vector<const AggregateFunction*> functions;
    std::vector<std::vector<Type>> types;
    std::size_t offset = query.group_by.size();
    for (const auto& e : aggregates) {
        auto arguments = std::span(shape.types).subspan(offset, e.arguments.size());
        types.emplace_back(arguments.begin(), arguments.end());
        offset += e.arguments.size();
        const auto& f = registry.aggregate(e.name);
        functions.push_back(&f);
        auto type = f.infer(types.back());
        registry.validate(type);
        data.types.push_back(type);
        if (e.distinct) {
            if (types.back().empty())
                fail(ErrorCode::schema, "DISTINCT aggregate needs arguments");
            for (const auto& t : types.back())
                if (!registry.orderable(t))
                    fail(ErrorCode::unsupported, "DISTINCT aggregate needs ordering");
        }
        if (!f.create(types.back(), registry))
            fail(ErrorCode::type, "Aggregate factory returned null");
    }
    auto working = tables;
    final.table = materialize(working, data);
    auto validate = final;
    validate.limit = 0;
    auto result = run(working, validate, registry);
    if (!query.limit)
        return result;
    using States = std::vector<std::unique_ptr<AggregateState>>;
    GroupTable<States> groups(registry, key_types, query.repeatable);
    auto states = [&]() {
        States out;
        for (std::size_t i = 0; i < functions.size(); ++i) {
            auto state = functions[i]->create(types[i], registry);
            if (!state)
                fail(ErrorCode::type, "Aggregate factory returned null");
            if (aggregates[i].distinct)
                state = std::make_unique<DistinctAggregate>(std::move(state), registry, types[i],
                                                            query.repeatable);
            out.push_back(std::move(state));
        }
        return out;
    };
    if (query.group_by.empty())
        groups.get({}, states);
    auto consume = [&](std::span<const Value> row) {
        auto keys = row.first(query.group_by.size());
        auto& group = groups.get(keys, states);
        std::size_t start = query.group_by.size();
        for (std::size_t i = 0; i < aggregates.size(); ++i) {
            group[i]->step(row.subspan(start, types[i].size()));
            start += types[i].size();
        }
    };
    if (query.repeatable) {
        // Projection failures have precedence over aggregate-step failures.
        // Buffer the first aggregate error while remaining projections run.
        // Nonrepeatable queries materialize input to preserve callback order.
        std::exception_ptr aggregate_error;
        RowConsumer stream = [&](std::span<const Value> row) {
            if (!aggregate_error) {
                try {
                    consume(row);
                } catch (...) {
                    aggregate_error = std::current_exception();
                }
            }
        };
        run_scan(tables, input, registry, {}, &stream);
        if (aggregate_error)
            std::rethrow_exception(aggregate_error);
    } else {
        for (const auto& row : run(tables, input, registry).rows)
            consume(row);
    }
    groups.finish([&](const Row& key, States& group) {
        Row row = key;
        for (std::size_t i = 0; i < group.size(); ++i) {
            auto value = group[i]->finish();
            registry.validate(value, data.types[query.group_by.size() + i]);
            row.push_back(std::move(value));
        }
        data.rows.push_back(std::move(row));
    });
    final.table = materialize(working, std::move(data));
    return run(working, final, registry);
}

Result run_distinct(const Tables& tables, const Query& query, const Registry& registry) {
    Query input = query;
    input.distinct = false;
    input.limit = query.limit ? std::numeric_limits<std::size_t>::max() : 0;
    auto result = run(tables, input, registry);
    for (const auto& t : result.types)
        if (!registry.orderable(t))
            fail(ErrorCode::unsupported, "DISTINCT needs orderable values");
    QueryRowLess less{registry, result.types, query.repeatable};
    std::set<Row, decltype(less)> seen(less);
    std::vector<Row> unique;
    for (auto& row : result.rows)
        if (seen.insert(row).second) {
            if (unique.size() < query.limit)
                unique.push_back(std::move(row));
        }
    result.rows = std::move(unique);
    return result;
}

} // namespace coresql::detail::execution
