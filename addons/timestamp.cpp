#include "coresql/timestamp.hpp"
#include "coresql/encoding.hpp"
#include <bit>

namespace coresql::timestamps {
namespace {
constexpr auto id = "coresql.timestamp.us";
void parameters(ByteView bytes) {
    if (!bytes.empty()) throw Error(ErrorCode::type, "Timestamp has no type parameters");
}
std::int64_t decode(ByteView bytes) {
    encoding::Reader reader(bytes);
    auto result = std::bit_cast<std::int64_t>(reader.u64());
    reader.end();
    return result;
}
}
Type type() { return {id, 1, {}}; }
Value value(std::int64_t unix_microseconds) {
    Bytes bytes;
    encoding::u64(bytes, std::bit_cast<std::uint64_t>(unix_microseconds));
    return Opaque(type(), std::move(bytes));
}
std::int64_t microseconds(const Value& value) {
    auto* opaque = std::get_if<Opaque>(&value);
    if (!opaque || opaque->type() != type()) throw Error(ErrorCode::type, "Expected timestamp value");
    return decode(opaque->bytes());
}
void install(Registry& registry) {
    registry.add(TypeAddon{id, 1, Representation::opaque, parameters,
        [](ByteView, const Value& v) { (void)decode(std::get<Opaque>(v).bytes()); },
        [](ByteView, const Value& a, const Value& b) { return decode(std::get<Opaque>(a).bytes()) == decode(std::get<Opaque>(b).bytes()); },
        [](ByteView, const Value& a, const Value& b) {
            auto x = decode(std::get<Opaque>(a).bytes()), y = decode(std::get<Opaque>(b).bytes());
            return (x > y) - (x < y);
        },
        [](ByteView, const Value& v) { return std::hash<std::int64_t>{}(decode(std::get<Opaque>(v).bytes())); }});
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
