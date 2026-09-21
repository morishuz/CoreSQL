#include "ast.hpp"

namespace coresql::sql::detail {
std::optional<Type> Lowerer::expression_type(const Expr& e) const {
    switch (e.kind) {
    case Expr::Kind::literal:
        return type_of(e.value);
    case Expr::Kind::row_id:
    case Expr::Kind::exists:
    case Expr::Kind::membership:
        return integer();
    case Expr::Kind::parameter: {
        if (e.name.starts_with("\x01sql.parameter.")) {
            const auto slot = std::stoull(e.name.substr(15));
            if (slot >= parameters.size())
                throw Error(ErrorCode::type, "Missing template parameter");
            return type_of(parameters[slot]);
        }
        if (auto it = parameter_types.find(e.name); it != parameter_types.end())
            return it->second;
        // Correlated captures retain the outer expression's type while lowering.
        // Keeping it avoids a dynamic SQL comparison around a typed predicate.
        if (outer && captures)
            for (const auto& [name, value] : *captures)
                if (name == e.name)
                    return outer->expression_type(value);
        return {};
    }
    case Expr::Kind::column:
        for (const auto& source : sources)
            if (e.qualifier.empty() || e.qualifier == source.alias)
                for (const auto& c : schema.at(source.table))
                    if (c.name == e.name)
                        return c.type;
        return {};
    case Expr::Kind::subquery: {
        if (!e.subquery)
            return {};
        const auto& q = *e.subquery;
        Schema expanded = schema;
        for (const auto& relation : q.relations)
            expanded[relation.name] = relation.columns;
        Lowerer nested{expanded, registry, parameters, {}};
        nested.types = types;
        if (!q.table.empty())
            nested.sources.push_back({q.table, q.alias.empty() ? q.table : q.alias});
        if (q.join)
            nested.sources.push_back({q.join->table, q.join->alias});
        for (const auto& j : q.joins)
            nested.sources.push_back({j.table, j.alias});
        for (std::size_t i = 0; i < e.parameters.size(); ++i) {
            auto type = expression_type(e.arguments[i]);
            if (type)
                nested.parameter_types.emplace(e.parameters[i], *type);
        }
        if (q.select.empty())
            return nested.sources.empty() ? std::nullopt
                                          : std::optional<Type>(schema.at(q.table).front().type);
        return nested.expression_type(q.select[0]);
    }
    case Expr::Kind::call:
    case Expr::Kind::aggregate: {
        std::vector<Type> types;
        for (const auto& arg : e.arguments) {
            auto type = expression_type(arg);
            if (!type)
                return {};
            types.push_back(*type);
        }
        return e.kind == Expr::Kind::call ? registry.function(e.name).infer(types)
                                          : registry.aggregate(e.name).infer(types);
    }
    case Expr::Kind::coalesce:
    case Expr::Kind::conditional: {
        std::vector<std::size_t> arms;
        if (e.kind == Expr::Kind::coalesce) {
            for (std::size_t i = 0; i < e.arguments.size(); ++i)
                arms.push_back(i);
        } else {
            std::size_t first = e.name.empty() ? 0 : 1;
            bool fallback = (e.arguments.size() - first) % 2 != 0;
            auto end = e.arguments.size() - (fallback ? 1 : 0);
            for (auto i = first + 1; i < end; i += 2)
                arms.push_back(i);
            if (fallback)
                arms.push_back(end);
        }
        for (auto i : arms)
            if (!(e.arguments[i].kind == Expr::Kind::literal && is_null(e.arguments[i].value)))
                return expression_type(e.arguments[i]);
        return integer();
    }
    }
    return {};
}
void Lowerer::unify(std::vector<Expr*>& arms) const {
    std::optional<Type> common;
    bool differs = false, known = true;
    for (auto e : arms) {
        if (e->kind == Expr::Kind::literal && is_null(e->value))
            continue;
        auto t = expression_type(*e);
        if (!t) {
            known = false;
            continue;
        }
        if (common && *common != *t)
            differs = true;
        common = t;
    }
    if (!known || !differs)
        return;
    std::vector<Type> types;
    for (auto e : arms)
        if (!(e->kind == Expr::Kind::literal && is_null(e->value)))
            types.push_back(*expression_type(*e));
    if (auto target = this->types->common_type(types)) {
        for (auto e : arms)
            *e = conversion(std::move(*e), *target);
        return;
    }

    throw Error(ErrorCode::type, "Result types have no common SQL type");
}
Expr Lowerer::truth(Expr e) const {
    auto type = expression_type(e);
    return type && *type == integer() ? std::move(e) : call("sql.truth", {std::move(e)});
}
} // namespace coresql::sql::detail
