#include "coresql/timestamp.hpp"

namespace coresql::timestamps {
namespace {
constexpr auto id = "coresql.timestamp.us";
void parameters(ByteView bytes) {
    if (!bytes.empty()) throw Error(ErrorCode::type, "Timestamp has no type parameters");
}
}
Type type() { return {id, 1, {}}; }
Value value(std::int64_t unix_microseconds) { return compact(type(), unix_microseconds); }
std::int64_t microseconds(const Value& value) {
    if (type_of(value) != type()) throw Error(ErrorCode::type, "Expected timestamp value");
    return i64_payload(value);
}
void install(Registry& registry) {
    registry.add(TypeAddon{id, 1, Layout::i64, parameters,
        [](ByteView, const Value& v) { (void)microseconds(v); },
        [](ByteView, const Value& a, const Value& b) { return microseconds(a) == microseconds(b); },
        [](ByteView, const Value& a, const Value& b) {
            auto x = microseconds(a), y = microseconds(b);
            return (x > y) - (x < y);
        },
        [](ByteView, const Value& v) { return std::hash<std::int64_t>{}(microseconds(v)); }, true});
    registry.add(Function{"timestamp.delta_us", [](std::span<const Type> types) {
        if (types.size() != 2 || types[0] != type() || types[1] != type())
            throw Error(ErrorCode::type, "timestamp.delta_us expects two timestamps");
        return integer();
    }, [](std::span<const Value> args) -> Value {
        auto a = microseconds(args[0]), b = microseconds(args[1]);
        if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b))
            throw Error(ErrorCode::type, "Timestamp difference overflows int64");
        return a - b;
    }});
}
} // namespace coresql::timestamps
