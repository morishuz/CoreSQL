#include "ast.hpp"
#include <set>

namespace coresql::sql::detail {
bool Lowerer::repeatable(const Select& s) const {
    // Nested SELECTs need their own scope. Analysis must not add captures to
    // the query being lowered merely to discover operation argument types.
    Lowerer local = *this;
    auto bindings = captures ? *captures : std::vector<std::pair<std::string, Expr>>{};
    if (captures)
        local.captures = &bindings;
    local.sources = s.sources;
    local.qualified = s.sources.size() > 1;
    for (auto& source : local.sources)
        if (auto cte = ctes.find(source.table); cte != ctes.end())
            source.table = cte->second;
    // SQL installs these names itself; arbitrary registered callbacks never opt in.
    static const std::set<std::string> functions{"text.substring",
                                                 "sum",
                                                 "avg",
                                                 "count",
                                                 "min",
                                                 "max",
                                                 "abs",
                                                 "length",
                                                 "coalesce",
                                                 "sql.cast_integer",
                                                 "sql.cast_real",
                                                 "sql.cast_text",
                                                 "sql.cast_numeric",
                                                 "sql.cast_type"};
    for (const auto& source : s.sources) {
        auto name = source.table;
        if (auto cte = ctes.find(name); cte != ctes.end())
            name = cte->second;
        if (source.query || !schema.contains(name))
            return false;
        for (const auto& c : schema.at(name))
            if (c.type != integer() && c.type != real() && c.type != text() && c.type != any_type() &&
                !(types->find(c.type) && types->find(c.type)->repeatable))
                return false;
    }
    std::function<bool(const Node&)> safe = [&](const Node& n) {
        if (n.kind == Node::function && n.name == "sql.cast_type" &&
            !(types->find(type_of(n.value)) && types->find(type_of(n.value))->repeatable))
            return false;
        if (n.kind == Node::function && !functions.contains(n.name)) {
            std::vector<Expr> args;
            for (const auto& arg : n.args) {
                args.push_back(local.expression(arg));
                if (!local.expression_type(args.back()))
                    return false;
            }
            auto op = local.operation(n.name, args, n.args);
            if (!op || !op->repeatable)
                return false;
        }
        if (n.query) {
            Lowerer nested = local;
            std::vector<std::pair<std::string, Expr>> nested_bindings;
            nested.outer = &local;
            nested.captures = &nested_bindings;
            if (!nested.repeatable(*n.query))
                return false;
        }
        return std::all_of(n.args.begin(), n.args.end(), safe);
    };
    auto optional = [&](const std::optional<Node>& n) { return !n || safe(*n); };
    for (const auto& source : s.sources)
        if (!optional(source.on))
            return false;
    for (const auto& [op, part] : s.compounds)
        if (!repeatable(*part))
            return false;
    return std::all_of(s.projection.begin(), s.projection.end(), safe) &&
           std::all_of(s.group.begin(), s.group.end(), safe) &&
           std::all_of(s.order.begin(), s.order.end(), [&](const auto& k) { return safe(k.first); }) &&
           optional(s.where) && optional(s.having) && optional(s.limit) && optional(s.offset);
}
} // namespace coresql::sql::detail

namespace coresql::sql::detail {
Query Lowerer::query(const Select& input, std::vector<Column>* output) const {
    Schema expanded = schema;
    Lowerer context{expanded, registry, parameters, sources, outer, captures, qualified, parameter_types};
    context.types = types;
    context.ctes = ctes;
    context.relation_id = relation_id;
    Select s = input;
    std::vector<Relation> relations;
    auto publish = [&](Query q, std::vector<Column> columns, const std::vector<std::string>& names) {
        if (!names.empty()) {
            if (names.size() != columns.size())
                throw Error(ErrorCode::schema, "Relation column-list width differs");
            for (std::size_t i = 0; i < names.size(); ++i)
                columns[i].name = names[i];
        }
        std::set<std::string> unique;
        for (const auto& c : columns)
            if (!unique.insert(c.name).second)
                throw Error(ErrorCode::schema, "Duplicate relation output name: " + c.name);
        std::string name = "\x01sql" + std::to_string((*relation_id)++);
        while (expanded.contains(name))
            name += '_';
        expanded[name] = columns;
        relations.push_back({name, std::make_shared<const Query>(std::move(q)), std::move(columns)});
        return name;
    };
    std::set<std::string> names;
    for (const auto& cte : s.ctes) {
        if (!names.insert(cte.name).second)
            throw Error(ErrorCode::schema, "Duplicate CTE name");
        context.ctes[cte.name] = ""; // Block self and forward references, including base-table fallback.
    }
    for (const auto& cte : s.ctes) {
        std::vector<Column> columns;
        auto q = context.query(*cte.query, &columns);
        context.ctes[cte.name] = publish(std::move(q), std::move(columns), cte.columns);
    }
    s.ctes.clear();
    for (auto& source : s.sources) {
        if (source.query) {
            auto child = context;
            child.outer = nullptr;
            child.captures = nullptr; // FROM relations are non-lateral.
            child.sources.clear();
            std::vector<Column> columns;
            auto q = child.query(*source.query, &columns);
            source.table = publish(std::move(q), std::move(columns), source.columns);
            source.query.reset();
        } else {
            if (auto found = context.ctes.find(source.table); found != context.ctes.end()) {
                if (found->second.empty())
                    throw Error(ErrorCode::unsupported, "Recursive or forward CTE reference");
                source.table = found->second;
            }
            if (!source.columns.empty()) {
                if (!expanded.contains(source.table))
                    throw Error(ErrorCode::schema, "Unknown relation");
                auto columns = expanded.at(source.table);
                source.table = publish(Query{source.table, {}, {}, {}}, std::move(columns), source.columns);
            }
        }
        source.columns.clear();
    }
    auto q = context.query_body(s);
    if (output) {
        auto local = context;
        local.sources = s.sources;
        local.qualified = s.sources.size() > 1;
        std::vector<std::string> labels;
        if (s.projection.size() == 1 && s.projection[0].kind == Node::star) {
            for (const auto& source : s.sources)
                for (const auto& c : expanded.at(source.table))
                    labels.push_back(c.name);
        } else {
            for (std::size_t i = 0; i < s.projection.size(); ++i)
                labels.push_back(!s.aliases[i].empty()                  ? s.aliases[i]
                                 : s.projection[i].kind == Node::column ? s.projection[i].name
                                                                        : "column" + std::to_string(i + 1));
        }
        for (std::size_t i = 0; i < q.select.size(); ++i) {
            auto type = local.expression_type(q.select[i]);
            if (!type)
                throw Error(ErrorCode::type, "Cannot infer relation output type");
            output->push_back({labels[i], *type, false, {}, {}, true});
        }
    }
    q.repeatable &= std::all_of(relations.begin(), relations.end(),
                                [](const Relation& r) { return r.query->repeatable; });
    q.relations = std::move(relations);
    return q;
}
} // namespace coresql::sql::detail
