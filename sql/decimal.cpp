#include "ast.hpp"
#include "coresql/decimal.hpp"

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
Expr Lowerer::decimal_literal(const Node& n, Expr e) const {
    auto spelling = numeric_spelling(n);
    if (!spelling.empty() && expression_type(e) == real())
        return literal(decimals::literal(spelling));
    return e;
}
Expr Lowerer::conversion(Expr e, const Type& target, bool explicit_cast) const {
    if (e.kind == Expr::Kind::literal && is_null(e.value))
        return literal(Null(target));
    if (decimals::is_decimal(target))
        return call(explicit_cast ? "sql.cast_decimal" : "sql.store_decimal",
                    {std::move(e), literal(Null(target))});
    auto name = target == integer() ? "integer"
                : target == real()  ? "real"
                : target == text()  ? "text"
                                    : "box";
    return call(std::string(explicit_cast ? "sql.cast_" : "sql.store_") + name, {std::move(e)});
}
Expr Lowerer::stored_expression(const Node& n, const Type& target, bool explicit_cast) const {
    auto e = expression(n);
    if (decimals::is_decimal(target)) {
        auto spelling = numeric_spelling(n);
        if (!spelling.empty())
            e = literal(spelling);
        return conversion(std::move(e), target, explicit_cast);
    }
    return e;
}
std::optional<Type> decimal_common(std::span<const Type> types) {
    if (std::none_of(types.begin(), types.end(), decimals::is_decimal))
        return {};
    bool floating = false;
    unsigned integral = 0, scale = 0;
    for (const auto& t : types) {
        if (t == real()) {
            floating = true;
            continue;
        }
        if (t == integer()) {
            integral = std::max(integral, 19u);
            continue;
        }
        if (!decimals::is_decimal(t))
            throw Error(ErrorCode::type, "DECIMAL result arms require numeric types");
        auto f = decimals::format_of(t);
        integral = std::max(integral, f.precision - f.scale);
        scale = std::max(scale, f.scale);
    }
    if (floating)
        return real();
    return decimals::type(integral + scale, scale);
}
void install_decimal_coercions(Registry& r) {
    for (bool explicit_cast : {false, true})
        r.add(Function{
            explicit_cast ? "sql.cast_decimal" : "sql.store_decimal",
            [](std::span<const Type> t) {
                if (t.size() != 2 || !decimals::is_decimal(t[1]) || !convertible(t[0], t[1]))
                    throw Error(ErrorCode::type, "Unsupported DECIMAL conversion");
                (void)decimals::format_of(t[1]);
                return t[1];
            },
            [explicit_cast](std::span<const Value> v) { return convert(v[0], type_of(v[1]), explicit_cast); },
            true});
}
} // namespace coresql::sql::detail
