#include "ast.hpp"
#include "membership.hpp"
#include "coresql/encoding.hpp"
#include <bit>
#include <cmath>

namespace coresql::sql::detail {
Type any_type() {
    return {"sql.value", 1, {}};
}
Value unpack(const Value& value) {
    auto opaque = std::get_if<Opaque>(&value);
    if (!opaque || opaque->type() != any_type())
        return value;
    encoding::Reader r(opaque->bytes());
    auto tag = std::to_integer<unsigned>(r.take(1)[0]);
    Value result;
    if (tag == 0)
        result = std::bit_cast<std::int64_t>(r.u64());
    else if (tag == 1) {
        auto d = std::bit_cast<double>(r.u64());
        if (!std::isfinite(d))
            throw Error(ErrorCode::format, "Invalid SQL real");
        result = d;
    } else if (tag == 2) {
        auto bytes = r.take(r.remaining());
        result = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    } else
        throw Error(ErrorCode::format, "Invalid SQL value tag");
    r.end();
    return result;
}
Value pack(const Value& value) {
    if (is_null(value))
        return Null(any_type());
    if (type_of(value) == any_type()) {
        unpack(value);
        return value;
    }
    Bytes b;
    if (auto n = std::get_if<std::int64_t>(&value)) {
        b.push_back(std::byte{0});
        encoding::u64(b, std::bit_cast<std::uint64_t>(*n));
    } else if (auto n = std::get_if<double>(&value)) {
        if (!std::isfinite(*n))
            throw Error(ErrorCode::type, "SQL real must be finite");
        b.push_back(std::byte{1});
        encoding::u64(b, std::bit_cast<std::uint64_t>(*n));
    } else if (auto s = std::get_if<std::string>(&value)) {
        b.push_back(std::byte{2});
        auto bytes = std::as_bytes(std::span(s->data(), s->size()));
        b.insert(b.end(), bytes.begin(), bytes.end());
    } else
        throw Error(ErrorCode::unsupported,
                    "Undeclared SQL columns support non-NULL integer, real and text values");
    return Opaque(any_type(), std::move(b));
}
std::string function_name(const std::string& op) {
    static const std::map<std::string, std::string> names = {
        {"is_null", "sql.is_null"},  {"length", "text.length"}, {"like", "text.like_ascii"},
        {"and", "integer.and"},      {"||", "sql.concat"},      {"=", "sql.equal"},
        {"==", "sql.equal"},         {"!=", "sql.not_equal"},   {"<>", "sql.not_equal"},
        {"<", "sql.less"},           {">", "sql.greater"},      {"<=", "sql.less_equal"},
        {">=", "sql.greater_equal"}, {"or", "sql.or"},          {"not", "sql.not"}};
    auto it = names.find(op);
    return it == names.end() ? op : it->second;
}
} // namespace coresql::sql::detail
namespace coresql::sql {
namespace {
int number_compare(std::int64_t i, double d) {
    if (d >= 9223372036854775808.0)
        return -1;
    if (d < -9223372036854775808.0)
        return 1;
    auto truncated = static_cast<std::int64_t>(d);
    if (i != truncated)
        return i < truncated ? -1 : 1;
    return double(truncated) < d ? -1 : double(truncated) > d ? 1 : 0;
}
int value_compare(const Value& input_a, const Value& input_b, const Registry& r) {
    auto a = detail::unpack(input_a), b = detail::unpack(input_b);
    if (type_of(a) == type_of(b))
        return r.compare(a, b);
    if (auto i = std::get_if<std::int64_t>(&a))
        if (auto d = std::get_if<double>(&b))
            return number_compare(*i, *d);
    if (auto d = std::get_if<double>(&a))
        if (auto i = std::get_if<std::int64_t>(&b))
            return -number_compare(*i, *d);
    return a.index() < b.index() ? -1 : 1;
}
} // namespace
void install(Registry& r, const TypeAdapters& adapters) {
    detail::install_string_functions(r);
    detail::install_scalar_policy(r);
    adapters.install(r);
    Registry comparisons = r;
    TypeAddon any;
    any.id = detail::any_type().id;
    any.validate_type = [](ByteView p) {
        if (!p.empty())
            throw Error(ErrorCode::type, "SQL value has no parameters");
    };
    any.validate_value = [](ByteView, const Value& v) { (void)detail::unpack(v); };
    any.compare = [comparisons](ByteView, const Value& a, const Value& b) {
        return value_compare(detail::unpack(a), detail::unpack(b), comparisons);
    };
    any.equal = [compare = any.compare](ByteView p, const Value& a, const Value& b) {
        return compare(p, a, b) == 0;
    };
    r.add(std::move(any));
    auto integers = [](std::span<const Type> t) {
        if (t.size() != 2 || t[0] != integer() || t[1] != integer())
            throw Error(ErrorCode::type, "Expected two integers");
        return integer();
    };
    detail::install_coercions(r, adapters);
    detail::install_type_conversions(r, adapters);
    comparisons = r;
    for (std::string op : {"equal", "not_equal", "less", "greater", "less_equal", "greater_equal"}) {
        Function function{
            "sql." + op,
            [comparisons, op](std::span<const Type> t) {
                if (t.size() != 2 || ((t[0] != t[1] || ((op == "equal" || op == "not_equal")
                                                            ? !comparisons.equatable(t[0])
                                                            : !comparisons.orderable(t[0]))) &&
                                      !(detail::scalar_type(t[0]) && detail::scalar_type(t[1]))))
                    throw Error(ErrorCode::type, "Comparison needs compatible types");
                return integer();
            },
            [comparisons, op](std::span<const Value> v) -> Value {
                if (op == "equal" || op == "not_equal") {
                    bool equal = type_of(v[0]) == type_of(v[1]) ? comparisons.equal(v[0], v[1])
                                                                : value_compare(v[0], v[1], comparisons) == 0;
                    return std::int64_t(op == "equal" ? equal : !equal);
                }
                int c = value_compare(v[0], v[1], comparisons);
                return std::int64_t(op == "less"         ? c < 0
                                    : op == "greater"    ? c > 0
                                    : op == "less_equal" ? c <= 0
                                                         : c >= 0);
            }};
        if (op == "equal")
            function.prepare_membership = detail::membership_factory(comparisons);
        r.add(std::move(function));
    }
    for (const auto& type : {integer(), real(), text()}) {
        Function function{detail::affinity_function(type, true),
                          [](std::span<const Type> t) {
                              if (t.size() != 2 || !detail::scalar_type(t[0]) || !detail::scalar_type(t[1]))
                                  throw Error(ErrorCode::type, "Affinity comparison needs SQL scalars");
                              return integer();
                          },
                          [type, comparisons, adapters](std::span<const Value> values) -> Value {
                              return std::int64_t(value_compare(detail::affinity(values[0], type, adapters),
                                                                detail::affinity(values[1], type, adapters),
                                                                comparisons) == 0);
                          }};
        function.prepare_membership = detail::membership_factory(comparisons, type);
        r.add(std::move(function));
    }
    r.add(Function{"sql.between",
                   [comparisons](std::span<const Type> t) {
                       if (t.size() != 3)
                           throw Error(ErrorCode::type, "BETWEEN needs three operands");
                       for (std::size_t i = 1; i < 3; ++i)
                           if (!((t[0] == t[i] && comparisons.orderable(t[0])) ||
                                 (detail::scalar_type(t[0]) && detail::scalar_type(t[i]))))
                               throw Error(ErrorCode::type, "BETWEEN needs compatible types");
                       return integer();
                   },
                   [comparisons](std::span<const Value> v) -> Value {
                       bool lower_null = is_null(v[0]) || is_null(v[1]),
                            upper_null = is_null(v[0]) || is_null(v[2]);
                       if (!lower_null && value_compare(v[0], v[1], comparisons) < 0)
                           return std::int64_t{0};
                       if (!upper_null && value_compare(v[0], v[2], comparisons) > 0)
                           return std::int64_t{0};
                       if (lower_null || upper_null)
                           return Null(integer());
                       return std::int64_t{1};
                   },
                   true});
    r.add(Function{"sql.is_null",
                   [](std::span<const Type> t) {
                       if (t.size() != 1)
                           throw Error(ErrorCode::type, "IS NULL needs one operand");
                       return integer();
                   },
                   [](std::span<const Value> v) -> Value { return std::int64_t(is_null(v[0])); }, true});
    r.add(Function{"sql.or", integers,
                   [](std::span<const Value> v) -> Value {
                       for (const auto& x : v)
                           if (!is_null(x) && std::get<std::int64_t>(x))
                               return std::int64_t{1};
                       if (is_null(v[0]) || is_null(v[1]))
                           return Null(integer());
                       return std::int64_t{0};
                   },
                   true});
    r.add(Function{
        "sql.not",
        [](std::span<const Type> t) {
            if (t.size() != 1 || t[0] != integer())
                throw Error(ErrorCode::type, "NOT needs integer");
            return integer();
        },
        [](std::span<const Value> v) -> Value { return std::int64_t(!std::get<std::int64_t>(v[0])); }});
}
} // namespace coresql::sql
