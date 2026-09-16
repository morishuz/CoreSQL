#include "coresql/core.hpp"

namespace coresql {
Predicate contains(Expr source, std::string extractor, Expr key) {
    Predicate result;
    result.kind = Predicate::Kind::contains;
    result.left = std::move(source);
    result.right = std::move(key);
    result.extractor = std::move(extractor);
    return result;
}
Predicate all_of(std::vector<Predicate> children) {
    Predicate result;
    result.kind = Predicate::Kind::all;
    result.children = std::move(children);
    return result;
}
Predicate any_of(std::vector<Predicate> children) {
    Predicate result;
    result.kind = Predicate::Kind::any;
    result.children = std::move(children);
    return result;
}
Predicate not_(Predicate child) {
    Predicate result;
    result.kind = Predicate::Kind::negation;
    result.children.push_back(std::move(child));
    return result;
}
Expr column(std::string qualifier, std::string name) {
    auto result = column(std::move(name));
    result.qualifier = std::move(qualifier);
    return result;
}
Expr column(std::string name) {
    return {Expr::Kind::column, std::move(name), std::int64_t{0}, {}};
}
Expr literal(Value value) {
    return {Expr::Kind::literal, {}, std::move(value), {}};
}
Expr choose(std::vector<std::pair<Expr, Expr>> branches, std::optional<Expr> otherwise) {
    Expr e{Expr::Kind::conditional, {}, {}, {}};
    for (auto& [condition, result] : branches) {
        e.arguments.push_back(std::move(condition));
        e.arguments.push_back(std::move(result));
    }
    if (otherwise)
        e.arguments.push_back(std::move(*otherwise));
    return e;
}
Expr choose(Expr base, std::string equality, std::vector<std::pair<Expr, Expr>> branches,
            std::optional<Expr> otherwise) {
    if (equality.empty())
        throw Error(ErrorCode::schema, "Matching conditional needs an equality function");
    auto e = choose(std::move(branches), std::move(otherwise));
    e.name = std::move(equality);
    e.arguments.insert(e.arguments.begin(), std::move(base));
    return e;
}
Expr coalesce(std::vector<Expr> arguments) {
    return {Expr::Kind::coalesce, {}, {}, std::move(arguments)};
}
Expr row_id() {
    return {Expr::Kind::row_id, {}, {}, {}};
}
Expr parameter(std::string name) {
    return {Expr::Kind::parameter, std::move(name), {}, {}};
}
Expr membership(Expr value, std::vector<Expr> candidates, std::string equality_function) {
    Expr e{Expr::Kind::membership, std::move(equality_function), std::int64_t{0}, {std::move(value)}};
    for (auto& candidate : candidates)
        e.arguments.push_back(std::move(candidate));
    return e;
}
Expr membership(Expr value, Query candidates, std::string equality_function,
                std::vector<std::pair<std::string, Expr>> bindings) {
    auto e = membership(std::move(value), std::vector<Expr>{}, std::move(equality_function));
    e.subquery = std::make_shared<const Query>(std::move(candidates));
    for (auto& [name, argument] : bindings) {
        e.parameters.push_back(std::move(name));
        e.arguments.push_back(std::move(argument));
    }
    return e;
}
Expr scalar_subquery(Query q, std::vector<std::pair<std::string, Expr>> bindings) {
    Expr e{Expr::Kind::subquery, {}, {}, {}};
    e.subquery = std::make_shared<const Query>(std::move(q));
    for (auto& [name, value] : bindings) {
        e.parameters.push_back(std::move(name));
        e.arguments.push_back(std::move(value));
    }
    return e;
}
Expr exists(Query q, std::vector<std::pair<std::string, Expr>> bindings) {
    auto e = scalar_subquery(std::move(q), std::move(bindings));
    e.kind = Expr::Kind::exists;
    return e;
}
Expr aggregate(std::string name, std::vector<Expr> arguments, bool distinct) {
    Expr result{Expr::Kind::aggregate, std::move(name), std::int64_t{0}, std::move(arguments)};
    result.distinct = distinct;
    return result;
}
Expr call(std::string name, std::vector<Expr> arguments) {
    return {Expr::Kind::call, std::move(name), std::int64_t{0}, std::move(arguments)};
}

} // namespace coresql
