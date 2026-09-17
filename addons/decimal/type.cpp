#include "coresql/decimal.hpp"
#include "coresql/encoding.hpp"
#include <array>
#include <bit>
#include <cmath>

#ifndef __SIZEOF_INT128__
#error "The DECIMAL add-on requires compiler support for 128-bit integers"
#endif

namespace coresql::decimals {
namespace {
// Confined to this add-on; neither the public ABI nor the core uses int128.
__extension__ using Wide = __int128;
__extension__ using Unsigned = unsigned __int128;
constexpr auto id = "coresql.decimal";
[[noreturn]] void overflow() {
    throw Error(ErrorCode::constraint, "DECIMAL precision overflow");
}
void validate_format(unsigned precision, unsigned scale) {
    if (precision < 1 || precision > 38 || scale > precision)
        throw Error(ErrorCode::type, "DECIMAL requires 1 <= precision <= 38 and 0 <= scale <= precision");
}
Format parameter_format(ByteView parameters) {
    if (parameters.size() != 2)
        throw Error(ErrorCode::type, "Invalid DECIMAL type");
    auto precision = std::to_integer<unsigned>(parameters[0]);
    auto scale = std::to_integer<unsigned>(parameters[1]);
    validate_format(precision, scale);
    return {precision, scale};
}
Wide power(unsigned n) {
    static constexpr auto powers = [] {
        std::array<Wide, 39> result{};
        result[0] = 1;
        for (std::size_t i = 1; i < result.size(); ++i)
            result[i] = result[i - 1] * 10;
        return result;
    }();
    if (n >= powers.size())
        overflow();
    return powers[n];
}
Wide coefficient(const Value& v) {
    if (auto i = std::get_if<std::int64_t>(&v))
        return *i;
    auto o = std::get_if<Opaque>(&v);
    if (!o || !is_decimal(o->type()))
        throw Error(ErrorCode::type, "Expected DECIMAL");
    auto f = format_of(o->type());
    encoding::Reader reader(o->bytes());
    auto lo = reader.u64(), hi = reader.u64();
    reader.end();
    auto n = std::bit_cast<Wide>((Unsigned(hi) << 64) | lo);
    auto bound = power(f.precision);
    if (n <= -bound || n >= bound)
        overflow();
    return n;
}
Value value(Wide n, const Type& t) {
    auto bound = power(format_of(t).precision);
    if (n <= -bound || n >= bound)
        overflow();
    auto bits = std::bit_cast<Unsigned>(n);
    Bytes bytes;
    bytes.reserve(16);
    encoding::u64(bytes, static_cast<std::uint64_t>(bits));
    encoding::u64(bytes, static_cast<std::uint64_t>(bits >> 64));
    return Opaque(t, std::move(bytes));
}
Format numeric(const Type& t) {
    if (t == integer())
        return {19, 0};
    return format_of(t);
}
Wide scaled(Wide n, unsigned places) {
    Wide out;
    if (__builtin_mul_overflow(n, power(places), &out))
        overflow();
    return out;
}
struct Digits {
    std::string digits;
    int scale = 0;
    bool negative = false;
};
Digits digits(std::string_view s) {
    if (s.empty() || s.size() > 256)
        throw Error(ErrorCode::type, "Invalid DECIMAL text length");
    Digits out;
    std::size_t i = 0;
    if (s[i] == '+' || s[i] == '-') {
        out.negative = s[i] == '-';
        ++i;
    }
    bool point = false;
    for (; i < s.size(); ++i) {
        auto c = s[i];
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        if (c < '0' || c > '9')
            break;
        out.digits += c;
        if (point)
            ++out.scale;
    }
    if (out.digits.empty())
        throw Error(ErrorCode::type, "DECIMAL requires digits");
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            negative = s[i] == '-';
            ++i;
        }
        auto start = i;
        int exponent = 0;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            exponent = exponent * 10 + (s[i] - '0');
            if (exponent > 1000)
                overflow();
        }
        if (i == start)
            throw Error(ErrorCode::type, "Invalid DECIMAL exponent");
        out.scale += negative ? exponent : -exponent;
    }
    if (i != s.size())
        throw Error(ErrorCode::type, "Invalid DECIMAL text");
    auto first = out.digits.find_first_not_of('0');
    out.digits = first == std::string::npos ? "0" : out.digits.substr(first);
    return out;
}
int compare(const Value& a, const Value& b) {
    if (type_of(a) == real() || type_of(b) == real()) {
        auto x = to_real(a), y = to_real(b);
        return (x > y) - (x < y);
    }
    auto x = coefficient(a), y = coefficient(b);
    auto sa = numeric(type_of(a)).scale, sb = numeric(type_of(b)).scale;
    if (sa > sb)
        return -compare(b, a);
    auto factor = power(sb - sa), q = y / factor, rem = y % factor;
    if (x != q)
        return (x > q) - (x < q);
    return (0 > rem) - (0 < rem);
}
void operands(std::span<const Type> t, std::size_t count) {
    if (t.size() != count)
        throw Error(ErrorCode::type, "Wrong DECIMAL argument count");
    for (const auto& v : t)
        if (v != integer() && v != real())
            (void)format_of(v);
}
Type arithmetic_type(std::span<const Type> t, std::string_view op) {
    operands(t, 2);
    if (op == "divide" || t[0] == real() || t[1] == real())
        return real();
    auto a = numeric(t[0]), b = numeric(t[1]);
    auto scale = op == "multiply" ? a.scale + b.scale : std::max(a.scale, b.scale);
    if (scale > 38)
        throw Error(ErrorCode::type, "DECIMAL result scale exceeds 38");
    auto precision = op == "multiply" ? a.precision + b.precision
                                      : std::max(a.precision - a.scale, b.precision - b.scale) + scale + 1;
    return type(std::min(38u, precision), scale);
}
// Type-dependent arithmetic setup is shared by direct invocation and binding.
// Binding owns the result type and scale factors for this expression only.
struct Arithmetic {
    Type result;
    std::string op;
    bool floating;
    unsigned sa = 0, sb = 0, scale = 0;
    Wide fa = 1, fb = 1, base = 1, limit = 1;
    Arithmetic(std::span<const Type> types, Type type, std::string operation)
        : result(std::move(type)), op(std::move(operation)), floating(result == real()) {
        if (!floating) {
            sa = numeric(types[0]).scale;
            sb = numeric(types[1]).scale;
            const auto format = format_of(result);
            scale = format.scale;
            fa = power(sa);
            fb = power(sb);
            base = power(scale);
            limit = power(format.precision - scale);
        }
    }
    Value operator()(std::span<const Value> v) const {
        if (floating) {
            auto a = to_real(v[0]), b = to_real(v[1]);
            if (op == "divide" && b == 0)
                return Null(real());
            double n = op == "add" ? a + b : op == "subtract" ? a - b : op == "multiply" ? a * b : a / b;
            if (!std::isfinite(n))
                overflow();
            return n;
        }
        auto a = coefficient(v[0]), b = coefficient(v[1]);
        Wide n = 0;
        if (op == "multiply") {
            if (__builtin_mul_overflow(a, b, &n))
                overflow();
        } else {
            // Combine integral/fractional parts before rescaling, so cancelling
            // operands can succeed even if either rescaled value exceeds int128.
            if (op == "subtract")
                b = -b;
            Wide integral, fraction;
            if (__builtin_add_overflow(a / fa, b / fb, &integral) ||
                __builtin_add_overflow(scaled(a % fa, scale - sa), scaled(b % fb, scale - sb), &fraction))
                overflow();
            if (__builtin_add_overflow(integral, fraction / base, &integral))
                overflow();
            fraction %= base;
            if (integral < -limit || integral > limit)
                overflow();
            if (__builtin_add_overflow(integral * base, fraction, &n))
                overflow();
        }
        return value(n, result);
    }
};
struct Sum final : AggregateState {
    Type result;
    bool average;
    Wide limit;
    Wide total = 0;
    std::int64_t count = 0;
    Sum(Type t, bool avg) : result(std::move(t)), average(avg), limit(power(format_of(result).precision)) {}
    void step(std::span<const Value> args) override {
        if (is_null(args[0]))
            return;
        if (count == INT64_MAX || __builtin_add_overflow(total, coefficient(args[0]), &total))
            overflow();
        if (total <= -limit || total >= limit)
            overflow();
        ++count;
    }
    Value finish() override {
        if (!count)
            return Null(average ? real() : result);
        auto v = value(total, result);
        return average ? Value(to_real(v) / static_cast<double>(count)) : v;
    }
};
} // namespace
Type type(unsigned p, unsigned s) {
    validate_format(p, s);
    return {id, 1, Bytes{std::byte(p), std::byte(s)}};
}
bool is_decimal(const Type& t) {
    return t.id == id && t.version == 1;
}
Format format_of(const Type& t) {
    if (!is_decimal(t))
        throw Error(ErrorCode::type, "Invalid DECIMAL type");
    return parameter_format(t.parameters);
}
Value parse(std::string_view input, const Type& target, bool round) {
    auto f = format_of(target);
    auto d = digits(input);
    bool up = false;
    int shift = static_cast<int>(f.scale) - d.scale;
    if (d.digits == "0")
        return value(0, target);
    if (shift >= 0) {
        if (d.digits.size() + static_cast<unsigned>(shift) > f.precision)
            overflow();
        d.digits.append(static_cast<unsigned>(shift), '0');
    } else {
        auto remove = static_cast<std::size_t>(-shift);
        if (remove > d.digits.size()) {
            if (!round)
                throw Error(ErrorCode::type, "Lossy DECIMAL conversion");
            d.digits = "0";
        } else {
            auto keep = d.digits.size() - remove;
            if (!round && d.digits.find_first_not_of('0', keep) != std::string::npos)
                throw Error(ErrorCode::type, "Lossy DECIMAL conversion");
            up = round && d.digits[keep] >= '5';
            d.digits.resize(keep);
        }
    }
    if (d.digits.size() > f.precision)
        overflow();
    Wide n = 0;
    for (char c : d.digits)
        n = n * 10 + (c - '0');
    if (up)
        ++n;
    return value(d.negative ? -n : n, target);
}
Value literal(std::string_view input) {
    auto d = digits(input);
    while (d.scale > 0 && d.digits.size() > 1 && d.digits.back() == '0') {
        d.digits.pop_back();
        --d.scale;
    }
    if (d.digits == "0")
        return value(0, type(1, 0));
    auto scale = std::max(0, d.scale);
    auto precision = std::max(static_cast<int>(d.digits.size()) + std::max(0, -d.scale), scale);
    if (precision > 38)
        overflow();
    return parse(input, type(static_cast<unsigned>(precision), static_cast<unsigned>(scale)));
}
std::string format(const Value& v) {
    auto n = coefficient(v);
    auto scale = numeric(type_of(v)).scale;
    bool negative = n < 0;
    if (negative)
        n = -n;
    std::string out;
    do {
        out += static_cast<char>('0' + n % 10);
        n /= 10;
    } while (n);
    while (out.size() <= scale)
        out += '0';
    std::reverse(out.begin(), out.end());
    if (scale)
        out.insert(out.size() - scale, 1, '.');
    if (negative)
        out.insert(out.begin(), '-');
    return out;
}
Value convert(const Value& v, const Type& target, bool round) {
    (void)format_of(target);
    if (is_null(v))
        return Null(target);
    if (auto s = std::get_if<std::string>(&v))
        return parse(*s, target, round);
    if (type_of(v) == integer() || is_decimal(type_of(v)))
        return parse(format(v), target, round);
    throw Error(ErrorCode::type,
                "DECIMAL conversion requires decimal, integer or text; cast REAL to TEXT explicitly");
}
double to_real(const Value& v) {
    if (auto d = std::get_if<double>(&v)) {
        if (!std::isfinite(*d))
            throw Error(ErrorCode::type, "REAL must be finite");
        return *d;
    }
    auto f = numeric(type_of(v));
    return static_cast<double>(coefficient(v)) / static_cast<double>(power(f.scale));
}
std::int64_t to_integer(const Value& v, bool truncate) {
    auto n = coefficient(v), factor = power(numeric(type_of(v)).scale);
    if (!truncate && n % factor)
        throw Error(ErrorCode::type, "Lossy integer conversion");
    n /= factor;
    if (n < INT64_MIN || n > INT64_MAX)
        overflow();
    return static_cast<std::int64_t>(n);
}
void install(Registry& r) {
    r.add(TypeAddon{id, 1, Representation::opaque, [](ByteView p) { (void)parameter_format(p); },
                    [](ByteView, const Value& v) { (void)coefficient(v); },
                    [](ByteView, const Value& a, const Value& b) { return coefficient(a) == coefficient(b); },
                    [](ByteView, const Value& a, const Value& b) { return compare(a, b); },
                    [](ByteView, const Value& v) {
                        auto bits = std::bit_cast<Unsigned>(coefficient(v));
                        return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(bits)) ^
                               std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(bits >> 64));
                    }});
    for (std::string op : {"add", "subtract", "multiply", "divide"}) {
        auto infer = [op](std::span<const Type> t) { return arithmetic_type(t, op); };
        r.add(Function{
            "decimal." + op, infer,
            [infer, op](std::span<const Value> v) -> Value {
                const std::array<Type, 2> types{type_of(v[0]), type_of(v[1])};
                return Arithmetic(types, infer(types), op)(v);
            },
            false,
            [op](std::span<const Type> types, const Type& result) { return Arithmetic(types, result, op); }});
    }

    for (std::string op : {"equal", "not_equal", "less", "greater", "less_equal", "greater_equal"})
        r.add(Function{"decimal." + op,
                       [](std::span<const Type> t) {
                           operands(t, 2);
                           return integer();
                       },
                       [op](std::span<const Value> v) -> Value {
                           auto c = compare(v[0], v[1]);
                           return std::int64_t(op == "equal"        ? c == 0
                                               : op == "not_equal"  ? c != 0
                                               : op == "less"       ? c < 0
                                               : op == "greater"    ? c > 0
                                               : op == "less_equal" ? c <= 0
                                                                    : c >= 0);
                       }});
    r.add(Function{"decimal.between",
                   [](std::span<const Type> t) {
                       operands(t, 3);
                       return integer();
                   },
                   [](std::span<const Value> v) -> Value {
                       bool lo = is_null(v[0]) || is_null(v[1]), hi = is_null(v[0]) || is_null(v[2]);
                       if (!lo && compare(v[0], v[1]) < 0)
                           return std::int64_t{0};
                       if (!hi && compare(v[0], v[2]) > 0)
                           return std::int64_t{0};
                       return lo || hi ? Value(Null(integer())) : Value(std::int64_t{1});
                   },
                   true});
    for (std::string op : {"negate", "abs"})
        r.add(Function{"decimal." + op,
                       [](std::span<const Type> t) {
                           if (t.size() != 1)
                               throw Error(ErrorCode::type, "Expected one DECIMAL");
                           (void)format_of(t[0]);
                           return t[0];
                       },
                       [op](std::span<const Value> v) {
                           auto n = coefficient(v[0]);
                           return value(op == "negate" || n < 0 ? -n : n, type_of(v[0]));
                       }});
    for (bool avg : {false, true}) {
        auto infer = [avg](std::span<const Type> t) {
            if (t.size() != 1)
                throw Error(ErrorCode::type, "Expected one DECIMAL");
            auto f = format_of(t[0]);
            return avg ? real() : type(38, f.scale);
        };
        r.add(AggregateFunction{
            avg ? "decimal.avg" : "decimal.sum", infer,
            [infer, avg](std::span<const Type> t, const Registry&) -> std::unique_ptr<AggregateState> {
                (void)infer(t);
                return std::make_unique<Sum>(type(38, format_of(t[0]).scale), avg);
            }});
    }
}
} // namespace coresql::decimals
