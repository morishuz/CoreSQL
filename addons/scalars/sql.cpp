#include "coresql/sql_types.hpp"
#include "../../sql/value_policy.hpp"
#include <charconv>
#include <array>
#include <cmath>
#include <limits>

namespace coresql::sql {
namespace {
using detail::any_type;
using detail::convert;
using detail::pack;
using detail::scalar_type;
using detail::unpack;
std::string_view trim(std::string_view s) {
    auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!s.empty() && space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && space(s.back()))
        s.remove_suffix(1);
    return s;
}
// Parsing is locale-independent. Explicit casts/arithmetic accept a numeric
// prefix; storage conversion and affinity require the whole string to be numeric.
std::optional<Value> number(std::string_view input, bool whole, bool integer_only = false) {
    auto s = trim(input);
    if (s.empty())
        return {};
    bool plus = s.front() == '+';
    if (plus)
        s.remove_prefix(1);
    if (s.empty() || (plus && (s.front() == '-' || s.front() == '+')))
        return {};
    std::int64_t i = 0;
    auto ir = std::from_chars(s.data(), s.data() + s.size(), i);
    if (ir.ec == std::errc{} && ir.ptr != s.data() &&
        ((integer_only && !whole) || ir.ptr == s.data() + s.size()))
        return i;
    if (integer_only && !whole) {
        if (ir.ec == std::errc::result_out_of_range)
            return s.front() == '-' ? INT64_MIN : INT64_MAX;
        return {};
    }
    double d = 0;
    auto dr = std::from_chars(s.data(), s.data() + s.size(), d, std::chars_format::general);
    if (dr.ec == std::errc::result_out_of_range)
        throw Error(ErrorCode::constraint, "Numeric text out of range");
    if (dr.ec != std::errc{} || dr.ptr == s.data() || (whole && dr.ptr != s.data() + s.size()) ||
        !std::isfinite(d))
        return {};
    return d;
}
std::int64_t integer_value(const Value& v) {
    if (auto i = std::get_if<std::int64_t>(&v))
        return *i;
    double d = std::get<double>(v);
    if (d >= 9223372036854775808.0)
        return INT64_MAX;
    if (d <= -9223372036854775808.0)
        return INT64_MIN;
    return static_cast<std::int64_t>(d);
}
double real_value(const Value& v) {
    if (auto d = std::get_if<double>(&v))
        return *d;
    return static_cast<double>(std::get<std::int64_t>(v));
}
Value numeric_value(Value v) {
    v = unpack(v);
    if (auto s = std::get_if<std::string>(&v))
        return number(*s, false).value_or(Value(std::int64_t{0}));
    return v;
}
std::string string_value(const Value& v) {
    if (auto s = std::get_if<std::string>(&v))
        return *s;
    if (auto i = std::get_if<std::int64_t>(&v))
        return std::to_string(*i);
    char buffer[128];
    auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), std::get<double>(v) == 0 ? 0.0 : std::get<double>(v),
                      std::chars_format::general, 15);
    if (result.ec != std::errc{})
        throw Error(ErrorCode::type, "Cannot format real");
    std::string out(buffer, result.ptr);
    if (out.find('.') == out.npos) {
        auto exponent = out.find_first_of("eE");
        if (exponent == out.npos)
            out += ".0";
        else
            out.insert(exponent, ".0");
    }
    return out;
}
Value convert_scalar(Value v, const Type& target, bool explicit_cast) {
    v = unpack(v);
    if (is_null(v))
        return Null(target);
    if (type_of(v) == target)
        return v;
    if (target == any_type())
        return pack(v);
    if (!scalar_type(type_of(v)) || !scalar_type(target))
        throw Error(ErrorCode::type, "Extension values require explicit extension conversion");
    if (target == text())
        return string_value(v);
    if (auto s = std::get_if<std::string>(&v)) {
        auto parsed = number(*s, !explicit_cast, explicit_cast && target == integer());
        if (!parsed && !explicit_cast)
            throw Error(ErrorCode::type, "Text is not a complete numeric value");
        v = parsed.value_or(Value(std::int64_t{0}));
    }
    if (target == real())
        return real_value(v);
    if (target == integer()) {
        auto i = integer_value(v);
        if (!explicit_cast && std::holds_alternative<double>(v)) {
            auto d = std::get<double>(v);
            if (d >= 9223372036854775808.0 || d < -9223372036854775808.0 || static_cast<double>(i) != d)
                throw Error(ErrorCode::type, "Lossy integer conversion");
        }
        return i;
    }
    throw Error(ErrorCode::type, "Unsupported conversion");
}
Value scalar_affinity(Value v, const Type& type) {
    v = unpack(v);
    if (is_null(v))
        return v;
    if (type == text())
        return convert_scalar(std::move(v), text(), false);
    if (auto s = std::get_if<std::string>(&v)) {
        auto n = number(*s, true);
        if (n)
            v = *n;
    }
    return v;
}
struct ConvertedAggregate final : AggregateState {
    std::unique_ptr<AggregateState> inner;
    Type input;
    ConvertedAggregate(std::unique_ptr<AggregateState> state, Type type)
        : inner(std::move(state)), input(std::move(type)) {}
    void step(std::span<const Value> values) override {
        std::array<Value, 1> args{convert(values[0], input, true)};
        inner->step(args);
    }
    Value finish() override { return inner->finish(); }
};
struct NumericSum final : AggregateState {
    bool present = false, floating = false;
    std::int64_t integral = 0;
    double total = 0;
    void step(std::span<const Value> values) override {
        if (is_null(values[0]))
            return;
        auto v = numeric_value(values[0]);
        present = true;
        if (!floating && std::holds_alternative<std::int64_t>(v)) {
            if (__builtin_add_overflow(integral, std::get<std::int64_t>(v), &integral))
                throw Error(ErrorCode::constraint, "Integer sum overflow");
        } else {
            if (!floating) {
                total = static_cast<double>(integral);
                floating = true;
            }
            total += real_value(v);
            if (!std::isfinite(total))
                throw Error(ErrorCode::constraint, "Real sum overflow");
        }
    }
    Value finish() override {
        return !present ? Value(Null(any_type())) : pack(floating ? Value(total) : Value(integral));
    }
};
void install_scalar_sql(Registry& r) {
    auto scalar = [](std::span<const Type> types, std::size_t n) {
        if (types.size() != n)
            throw Error(ErrorCode::type, "Wrong scalar argument count");
        for (const auto& t : types)
            if (!scalar_type(t))
                throw Error(ErrorCode::type, "Expected SQL scalar value");
    };
    r.add(Function{"sql.cast_numeric",
                   [scalar](std::span<const Type> t) {
                       scalar(t, 1);
                       return any_type();
                   },
                   [](std::span<const Value> args) {
                       auto v = numeric_value(args[0]);
                       bool from_text = std::holds_alternative<std::string>(unpack(args[0]));
                       if (auto d = std::get_if<double>(&v); from_text && d && *d >= -9223372036854775808.0 &&
                                                             *d < 9223372036854775808.0 &&
                                                             std::trunc(*d) == *d)
                           v = static_cast<std::int64_t>(*d);
                       return pack(v);
                   }});
    r.add(Function{"sql.truth",
                   [scalar](std::span<const Type> t) {
                       scalar(t, 1);
                       return integer();
                   },
                   [](std::span<const Value> v) -> Value {
                       return std::int64_t(real_value(numeric_value(v[0])) != 0);
                   }});
    r.add(Function{"sql.concat",
                   [scalar](std::span<const Type> t) {
                       scalar(t, 2);
                       return text();
                   },
                   [](std::span<const Value> v) -> Value {
                       return string_value(unpack(v[0])) + string_value(unpack(v[1]));
                   }});
    for (std::string op : {"add", "subtract", "multiply", "divide", "remainder", "bitand"}) {
        auto infer = [scalar, op](std::span<const Type> t) {
            scalar(t, 2);
            if (op == "bitand")
                return integer();
            if (t[0] == any_type() || t[1] == any_type() || t[0] == text() || t[1] == text())
                return any_type();
            return t[0] == real() || t[1] == real() ? real() : integer();
        };
        r.add(Function{"sql.numeric_" + op, infer, [infer, op](std::span<const Value> v) -> Value {
                           auto target = infer(std::array<Type, 2>{type_of(v[0]), type_of(v[1])});
                           auto a = numeric_value(v[0]), b = numeric_value(v[1]);
                           Value out;
                           if (op == "bitand")
                               return integer_value(a) & integer_value(b);
                           if ((op == "divide" || op == "remainder") && real_value(b) == 0)
                               return Null(target);
                           if (op == "remainder") {
                               auto x = integer_value(a), y = integer_value(b);
                               if (!y)
                                   return Null(target);
                               auto n = x == INT64_MIN && y == -1 ? 0 : x % y;
                               out = std::holds_alternative<double>(a) || std::holds_alternative<double>(b)
                                         ? Value(double(n))
                                         : Value(n);
                           } else if (std::holds_alternative<std::int64_t>(a) &&
                                      std::holds_alternative<std::int64_t>(b)) {
                               auto x = std::get<std::int64_t>(a), y = std::get<std::int64_t>(b),
                                    n = std::int64_t{};
                               bool overflow = false;
                               if (op == "add")
                                   overflow = __builtin_add_overflow(x, y, &n);
                               else if (op == "subtract")
                                   overflow = __builtin_sub_overflow(x, y, &n);
                               else if (op == "multiply")
                                   overflow = __builtin_mul_overflow(x, y, &n);
                               else {
                                   overflow = x == INT64_MIN && y == -1;
                                   if (!overflow)
                                       n = x / y;
                               }
                               if (overflow)
                                   throw Error(ErrorCode::constraint, "Integer overflow");
                               out = n;
                           } else {
                               auto x = real_value(a), y = real_value(b);
                               auto d = op == "add"        ? x + y
                                        : op == "subtract" ? x - y
                                        : op == "multiply" ? x * y
                                                           : x / y;
                               if (!std::isfinite(d))
                                   throw Error(ErrorCode::constraint, "Real overflow");
                               out = d;
                           }
                           return target == any_type() ? pack(out) : convert(out, target);
                       }});
    }
    r.add(AggregateFunction{
        "sql.sum",
        [scalar](std::span<const Type> t) {
            scalar(t, 1);
            return any_type();
        },
        [scalar](std::span<const Type> t, const Registry&) -> std::unique_ptr<AggregateState> {
            scalar(t, 1);
            return std::make_unique<NumericSum>();
        }});
    for (std::string name : {"avg", "group_concat"}) {
        auto base = r.aggregate(name);
        auto input = name == "group_concat" ? text() : real();
        auto infer = [base, input, scalar](std::span<const Type> types) {
            scalar(types, 1);
            return base.infer(std::array<Type, 1>{input});
        };
        r.add(AggregateFunction{"sql." + name, infer,
                                [base, input, infer](std::span<const Type> types, const Registry& registry)
                                    -> std::unique_ptr<AggregateState> {
                                    (void)infer(types);
                                    auto inner = base.create(std::array<Type, 1>{input}, registry);
                                    if (!inner)
                                        throw Error(ErrorCode::type, "Aggregate factory returned null");
                                    return std::make_unique<ConvertedAggregate>(std::move(inner), input);
                                }});
    }
    for (std::string op : {"negate", "abs"})
        r.add(Function{"sql.numeric_" + op,
                       [scalar, op](std::span<const Type> t) {
                           scalar(t, 1);
                           if (t[0] == text() && op == "abs")
                               return real();
                           return t[0] == text() || t[0] == any_type() ? any_type() : t[0];
                       },
                       [op](std::span<const Value> args) {
                           auto raw = unpack(args[0]);
                           auto v = numeric_value(raw);
                           if (op == "abs" && std::holds_alternative<std::string>(raw))
                               v = std::fabs(real_value(v));
                           else if (auto d = std::get_if<double>(&v))
                               v = op == "abs" ? std::fabs(*d) : -*d;
                           else {
                               auto i = std::get<std::int64_t>(v);
                               if (i == INT64_MIN)
                                   throw Error(ErrorCode::constraint, "Integer overflow");
                               v = op == "abs" && i >= 0 ? i : -i;
                           }
                           return type_of(args[0]) == any_type() ||
                                          (type_of(args[0]) == text() && op != "abs")
                                      ? pack(v)
                                      : v;
                       }});
}

SqlTypeAdapter scalar_adapter(Type type, std::vector<std::string> names) {
    SqlTypeAdapter a;
    a.type_id = type.id;
    a.names = std::move(names);
    a.declaration = [type](const SqlDeclaration& d) {
        if (!d.parameters.empty() &&
            (d.name != "varchar" || d.explicit_cast || d.parameters.size() != 1 || !d.parameters[0]))
            throw Error(ErrorCode::unsupported, "Invalid scalar type parameters");
        return type;
    };
    a.install = [](Registry&) {}; // Registry already supplies the backend scalar types.
    a.can_convert = [type](const Type& source, const Type& target) {
        return target == type && scalar_type(source);
    };
    a.convert = convert_scalar;
    a.affinity = type == text() ? SqlAffinity::text : SqlAffinity::numeric;
    a.apply_affinity = [type](const Value& v) { return scalar_affinity(v, type); };
    a.repeatable = true;
    a.reorder_comparisons = true;
    return a;
}
} // namespace
SqlTypeAdapter integer_adapter() {
    return scalar_adapter(integer(), {"integer", "int", "bigint"});
}
namespace detail {
void install_scalar_policy(Registry& registry) {
    install_scalar_sql(registry);
}
std::optional<SqlOperation> scalar_operation(std::string_view op, std::span<const Type> types) {
    // Bare NULL has SQL's default integer context unless a domain adapter claimed it.
    auto scalar = [](const Type& t) { return t.id.empty() || scalar_type(t); };
    if (types.empty() || !std::all_of(types.begin(), types.end(), scalar))
        return {};
    if (types.size() == 2 && types[0] == integer() && types[1] == integer()) {
        if (op == "+")
            return SqlOperation{"integer.add"};
        if (op == "*")
            return SqlOperation{"integer.multiply"};
        if (op == "&")
            return SqlOperation{"integer.bitand"};
    }
    static const std::map<std::string_view, std::string> names{
        {"+", "add"},       {"-", "subtract"}, {"*", "multiply"},    {"/", "divide"},
        {"%", "remainder"}, {"&", "bitand"},   {"negate", "negate"}, {"abs", "abs"}};
    if (auto it = names.find(op); it != names.end())
        return SqlOperation{"sql.numeric_" + it->second};
    if (types.size() == 1 &&
        (((op == "sum" || op == "avg") && (types[0] == text() || types[0] == any_type())) ||
         (op == "group_concat" && (types[0] == real() || types[0] == any_type()))))
        return SqlOperation{"sql." + std::string(op)};
    return {};
}
std::optional<Type> scalar_common_type(std::span<const Type> types) {
    if (!types.empty() && std::all_of(types.begin(), types.end(), scalar_type))
        return std::all_of(types.begin(), types.end(), [&](const Type& t) { return t == types[0]; })
                   ? types[0]
                   : any_type();
    return {};
}
} // namespace detail
SqlTypeAdapter real_adapter() {
    return scalar_adapter(real(), {"real"});
}
SqlTypeAdapter text_adapter() {
    return scalar_adapter(text(), {"text", "varchar"});
}
} // namespace coresql::sql
