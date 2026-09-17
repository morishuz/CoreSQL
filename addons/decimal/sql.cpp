#include "coresql/sql_types.hpp"
#include "coresql/decimal.hpp"

namespace coresql::sql {
SqlTypeAdapter decimal_adapter() {
    SqlTypeAdapter a;
    a.type_id = decimals::type().id;
    a.names = {"decimal"};
    a.declaration = [](const SqlDeclaration& declaration) {
        auto p = declaration.parameters;
        if (p.size() > 2 || std::any_of(p.begin(), p.end(), [](auto n) { return n > UINT32_MAX; }))
            throw Error(ErrorCode::unsupported, "DECIMAL accepts precision and scale");
        return p.empty() ? decimals::type()
                         : decimals::type(static_cast<unsigned>(p[0]),
                                          p.size() == 2 ? static_cast<unsigned>(p[1]) : 0);
    };
    a.install = decimals::install;
    a.can_convert = [](const Type& s, const Type& t) {
        return (decimals::is_decimal(t) &&
                (decimals::is_decimal(s) || s == integer() || s == text() || s.id == "sql.value")) ||
               (decimals::is_decimal(s) && (t == integer() || t == real() || t == text()));
    };
    a.convert = [](const Value& v, const Type& t, bool cast) -> Value {
        if (decimals::is_decimal(t))
            return decimals::convert(v, t, cast);
        if (t == text())
            return decimals::format(v);
        if (t == real())
            return decimals::to_real(v);
        return decimals::to_integer(v, cast);
    };
    a.operation = [](std::string_view op, std::span<const Type> types) -> std::optional<SqlOperation> {
        if (std::none_of(types.begin(), types.end(), decimals::is_decimal))
            return {};
        static const std::map<std::string_view, std::string> names{
            {"+", "add"},           {"-", "subtract"},    {"*", "multiply"},    {"/", "divide"},
            {"=", "equal"},         {"==", "equal"},      {"!=", "not_equal"},  {"<>", "not_equal"},
            {"<", "less"},          {">", "greater"},     {"<=", "less_equal"}, {">=", "greater_equal"},
            {"between", "between"}, {"negate", "negate"}, {"abs", "abs"},       {"sum", "sum"},
            {"avg", "avg"}};
        auto it = names.find(op);
        if (it == names.end())
            return {};
        return SqlOperation{"decimal." + it->second, decimals::literal};
    };
    a.common_type = [](std::span<const Type> types) -> std::optional<Type> {
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
        return floating ? real() : decimals::type(integral + scale, scale);
    };
    a.exact_numeric_input = true;
    a.repeatable = true;
    return a;
}
} // namespace coresql::sql
