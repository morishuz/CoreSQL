#include "coresql/vector.hpp"
#include "coresql/encoding.hpp"
#include <bit>
#include <cmath>

namespace coresql::vectors {
namespace {
constexpr auto id = "coresql.vector.f32";
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
std::optional<std::uint64_t> dimensions(ByteView parameters) {
    if (parameters.empty()) return {};
    encoding::Reader reader(parameters);
    auto n = reader.u64();
    reader.end();
    return n;
}
void validate(ByteView parameters, ByteView bytes) {
    if (bytes.size() % 4) throw Error(ErrorCode::type, "Vector payload is not float32 aligned");
    if (auto n = dimensions(parameters); n && *n != bytes.size() / 4)
        throw Error(ErrorCode::type, "Vector dimension differs from schema");
    encoding::Reader reader(bytes);
    while (reader.remaining()) {
        if (!std::isfinite(std::bit_cast<float>(reader.u32())))
            throw Error(ErrorCode::type, "Vector contains a non-finite element");
    }
}
double distance(std::span<const Value> arguments) {
    const auto& a = std::get<Opaque>(arguments[0]);
    const auto& b = std::get<Opaque>(arguments[1]);
    if (a.bytes().size() != b.bytes().size()) throw Error(ErrorCode::type, "Distance requires equal vector lengths");
    encoding::Reader x(a.bytes()), y(b.bytes());
    double sum = 0;
    while (x.remaining()) {
        double delta = double(std::bit_cast<float>(x.u32())) - double(std::bit_cast<float>(y.u32()));
        sum += delta * delta;
    }
    return sum;
}
}
std::optional<std::uint64_t> dimensions(const Type& type) {
    if (type.id != id || type.version != 1) throw Error(ErrorCode::type, "Expected vector type");
    return dimensions(type.parameters);
}
Type type(std::optional<std::uint64_t> dimensions) {
    Bytes parameters;
    if (dimensions) encoding::u64(parameters, *dimensions);
    return {id, 1, std::move(parameters)};
}
Value value(std::span<const float> values, std::optional<std::uint64_t> n) {
    if (n && *n != values.size()) throw Error(ErrorCode::type, "Vector dimension differs from declaration");
    Bytes bytes;
    bytes.reserve(values.size_bytes());
    for (auto f : values) {
        if (!std::isfinite(f)) throw Error(ErrorCode::type, "Vector contains a non-finite element");
        encoding::u32(bytes, std::bit_cast<std::uint32_t>(f));
    }
    return Opaque(type(n), std::move(bytes));
}
std::vector<float> elements(const Value& value) {
    auto* opaque = std::get_if<Opaque>(&value);
    if (!opaque || opaque->type().id != id || opaque->type().version != 1)
        throw Error(ErrorCode::type, "Expected vector value");
    validate(opaque->type().parameters, opaque->bytes());
    encoding::Reader reader(opaque->bytes());
    std::vector<float> result;
    result.reserve(opaque->bytes().size() / 4);
    while (reader.remaining()) result.push_back(std::bit_cast<float>(reader.u32()));
    return result;
}
void install(Registry& registry) {
    registry.add(EncodedTypeAddon{id, 1,
        [](ByteView parameters) { (void)dimensions(parameters); }, validate,
        [](ByteView, ByteView a, ByteView b) {
            if (a.size() != b.size()) return false;
            encoding::Reader x(a), y(b);
            while (x.remaining()) if (std::bit_cast<float>(x.u32()) != std::bit_cast<float>(y.u32())) return false;
            return true;
        }, {}});
    registry.add(Function{"vector.squared_l2", [](std::span<const Type> types) {
        if (types.size() != 2) throw Error(ErrorCode::type, "vector.squared_l2 expects two vectors");
        for (const auto& t : types) if (t.id != id || t.version != 1)
            throw Error(ErrorCode::type, "vector.squared_l2 expects float32 vectors");
        auto a = dimensions(types[0].parameters), b = dimensions(types[1].parameters);
        if (a && b && a != b) throw Error(ErrorCode::type, "Distance dimension parameters differ");
        return real();
    }, [](std::span<const Value> arguments) -> Value { return distance(arguments); }});
}
} // namespace coresql::vectors
