#include "ast.hpp"

namespace coresql::sql::detail {
std::string numeric_spelling(const Node& n) {
    if (n.kind == Node::literal)
        return n.numeric_spelling;
    if (n.kind == Node::unary && n.name == "-" && n.args.size() == 1) {
        auto s = numeric_spelling(n.args[0]);
        if (!s.empty())
            return s.front() == '-' ? s.substr(1) : "-" + s;
    }
    return {};
}
std::optional<SqlOperation> Lowerer::operation(std::string_view op, std::span<const Expr> args,
                                               std::span<const Node> syntax) const {
    std::vector<Type> input;
    for (std::size_t i = 0; i < args.size(); ++i) {
        bool bare_null =
            syntax.size() == args.size() && syntax[i].kind == Node::literal && is_null(syntax[i].value);
        input.push_back(bare_null ? Type{} : expression_type(args[i]).value_or(integer()));
    }
    return types->operation(op, input);
}
Expr Lowerer::contextual_literal(const Node& n, Expr e, const SqlOperation& op) const {
    auto spelling = numeric_spelling(n);
    if (op.numeric_literal && !spelling.empty() && expression_type(e) == real())
        return literal(op.numeric_literal(spelling));
    return e;
}
Expr Lowerer::conversion(Expr e, const Type& target, bool explicit_cast) const {
    if (e.kind == Expr::Kind::literal && is_null(e.value))
        return literal(Null(target));
    if (types->find(target))
        return call(explicit_cast ? "sql.cast_type" : "sql.store_type",
                    {std::move(e), literal(Null(target))});
    if (target != any_type())
        throw Error(ErrorCode::type, "SQL target type has no adapter");
    return call(explicit_cast ? "sql.cast_box" : "sql.store_box", {std::move(e)});
}
namespace {
Expr storage_input(const Lowerer& lower, const Node& n, const Type& target) {
    auto e = lower.expression(n);
    if (auto adapter = lower.types->find(target); adapter && adapter->exact_numeric_input) {
        auto spelling = numeric_spelling(n);
        if (!spelling.empty())
            e = literal(spelling);
    }
    return e;
}
} // namespace
Expr Lowerer::stored_expression(const Node& n, const Type& target, bool explicit_cast) const {
    auto e = storage_input(*this, n, target);
    if (expression_type(e) == target)
        return e;
    return types->find(target) ? conversion(std::move(e), target, explicit_cast) : e;
}
Value Lowerer::stored_constant(const Node& n, const Type& target) const {
    // VALUES already evaluates each cell once. Convert its validated result
    // directly instead of constructing and binding a temporary conversion call.
    return convert(evaluate_constant(storage_input(*this, n, target), registry), target, false, *types);
}
void install_type_conversions(Registry& r, const TypeAdapters& types) {
    for (bool explicit_cast : {false, true})
        r.add(Function{explicit_cast ? "sql.cast_type" : "sql.store_type",
                       [types](std::span<const Type> t) {
                           if (t.size() != 2 || !types.find(t[1]) || !convertible(t[0], t[1], types))
                               throw Error(ErrorCode::type, "Unsupported SQL type conversion");
                           return t[1];
                       },
                       [types, explicit_cast](std::span<const Value> v) {
                           return convert(v[0], type_of(v[1]), explicit_cast, types);
                       },
                       true});
}
} // namespace coresql::sql::detail
