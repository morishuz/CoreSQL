#include "coresql/core.hpp"
#include <cmath>

namespace coresql {
namespace {
enum class Kind { count, sum, average, minimum, maximum, concat };
struct Reduction final : AggregateState {
    Kind kind;
    Type type;
    TypeAddon addon;
    std::int64_t count = 0, integer_sum = 0;
    double real_sum = 0;
    Value extremum;
    std::string joined;
    Reduction(Kind k, Type t, TypeAddon a) : kind(k), type(std::move(t)), addon(std::move(a)), extremum(Null(type)) {}
    void step(std::span<const Value> args) override {
        if (!args.empty() && is_null(args[0])) return;
        if (count == INT64_MAX) throw Error(ErrorCode::constraint, "Aggregate count overflow");
        ++count;
        if (kind == Kind::count) return;
        const auto& v = args[0];
        switch (kind) {
        case Kind::sum:
            if (type == integer()) {
                auto n = std::get<std::int64_t>(v);
                if ((n > 0 && integer_sum > INT64_MAX-n) || (n < 0 && integer_sum < INT64_MIN-n))
                    throw Error(ErrorCode::constraint, "Integer sum overflow");
                integer_sum += n; break;
            }
            [[fallthrough]];
        case Kind::average:
            real_sum += std::holds_alternative<double>(v) ? std::get<double>(v) : static_cast<double>(std::get<std::int64_t>(v));
            if (!std::isfinite(real_sum)) throw Error(ErrorCode::constraint, "Real aggregate overflow");
            break;
        case Kind::minimum: case Kind::maximum:
            if (is_null(extremum) || (kind == Kind::minimum ? addon.compare(type.parameters, v, extremum) < 0 : addon.compare(type.parameters, v, extremum) > 0)) extremum = v;
            break;
        case Kind::concat:
            if (count > 1) joined += ',';
            if (auto s = std::get_if<std::string>(&v)) joined += *s;
            else joined += std::to_string(std::get<std::int64_t>(v));
            break;
        default: break;
        }
    }
    Value finish() override {
        if (kind == Kind::count) return count;
        if (!count) return Null(type);
        switch (kind) {
        case Kind::sum: return type == integer() ? Value(integer_sum) : Value(real_sum);
        case Kind::average: return real_sum / static_cast<double>(count);
        case Kind::concat: return joined;
        default: return extremum;
        }
    }
};
}
void install_aggregate_functions(Registry& registry) {
    for (auto [name, kind] : {std::pair{"count", Kind::count}, {"sum", Kind::sum}, {"avg", Kind::average},
                             {"min", Kind::minimum}, {"max", Kind::maximum}, {"group_concat", Kind::concat}}) {
        auto infer = [kind](std::span<const Type> args) -> Type {
            if (kind == Kind::count && args.empty()) return integer();
            if (args.size() != 1) throw Error(ErrorCode::schema, "Aggregate requires one argument");
            const auto& t = args[0];
            if (kind == Kind::count) return integer();
            if (kind == Kind::sum || kind == Kind::average) {
                if (t != integer() && t != real()) throw Error(ErrorCode::type, "Numeric aggregate needs numeric input");
                return kind == Kind::average ? real() : t;
            }
            if (kind == Kind::concat) {
                if (t != text() && t != integer()) throw Error(ErrorCode::type, "Concatenation needs text or integer input");
                return text();
            }
            return t;
        };
        registry.add(AggregateFunction{name, infer, [kind, infer](std::span<const Type> args, const Registry& types) {
            auto result = infer(args);
            if ((kind == Kind::minimum || kind == Kind::maximum) && !types.orderable(result))
                throw Error(ErrorCode::unsupported, "Extremum needs ordering");
            return std::make_unique<Reduction>(kind, result, types.addon(result));
        }});
    }
}
}
