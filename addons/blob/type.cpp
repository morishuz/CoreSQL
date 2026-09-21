#include "coresql/blob.hpp"
namespace coresql::blobs {
const Type& type() {
    static const Type identity{"coresql.blob", 1, {}};
    return identity;
}
Value value(ByteView data) {
    return Opaque(type(), Bytes(data.begin(), data.end()));
}
ByteView bytes(const Value& value) {
    const auto* v = std::get_if<Opaque>(&value);
    if (!v || v->type() != type())
        throw Error(ErrorCode::type, "Expected BLOB");
    return v->bytes();
}
Value from_hex(std::string_view input) {
    if (input.size() % 2)
        throw Error(ErrorCode::type, "BLOB hex requires pairs of digits");
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9')
            return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<unsigned>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
            return static_cast<unsigned>(c - 'A' + 10);
        throw Error(ErrorCode::type, "Invalid BLOB hex digit");
    };
    Bytes data;
    data.reserve(input.size() / 2);
    for (std::size_t i = 0; i < input.size(); i += 2)
        data.push_back(std::byte((digit(input[i]) << 4) | digit(input[i + 1])));
    return Opaque(type(), std::move(data));
}
std::string hex(const Value& value) {
    constexpr char digits[] = "0123456789ABCDEF";
    const auto data = bytes(value);
    if (data.size() > std::string{}.max_size() / 2)
        throw Error(ErrorCode::resource, "BLOB hex is too large");
    std::string result;
    result.reserve(data.size() * 2);
    for (auto b : data) {
        const auto n = std::to_integer<unsigned>(b);
        result.push_back(digits[n >> 4]);
        result.push_back(digits[n & 15]);
    }
    return result;
}
void install(Registry& registry) {
    registry.add(TypeAddon{
        type().id, 1, Layout::bytes,
        [](ByteView p) {
            if (!p.empty())
                throw Error(ErrorCode::type, "BLOB takes no parameters");
        },
        [](ByteView, const Value& v) { (void)bytes(v); },
        [](ByteView, const Value& a, const Value& b) { return std::ranges::equal(bytes(a), bytes(b)); },
        [](ByteView, const Value& a, const Value& b) {
            const auto x = bytes(a), y = bytes(b);
            if (std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end()))
                return -1;
            return std::lexicographical_compare(y.begin(), y.end(), x.begin(), x.end()) ? 1 : 0;
        },
        [](ByteView, const Value& v) {
            std::size_t hash = 14695981039346656037ULL;
            for (auto b : bytes(v)) {
                hash ^= std::to_integer<unsigned>(b);
                hash *= 1099511628211ULL;
            }
            return hash;
        }});
    auto input = [](std::span<const Type> types) {
        if (types.size() != 1 || types[0] != type())
            throw Error(ErrorCode::type, "Expected one BLOB argument");
    };
    registry.add(Function{"blob.length",
                          [input](auto types) {
                              input(types);
                              return integer();
                          },
                          [](auto values) -> Value {
                              const auto size = bytes(values[0]).size();
                              if (size > INT64_MAX)
                                  throw Error(ErrorCode::resource, "BLOB length exceeds INTEGER");
                              return static_cast<std::int64_t>(size);
                          }});
    registry.add(Function{"blob.hex",
                          [input](auto types) {
                              input(types);
                              return text();
                          },
                          [](auto values) -> Value { return hex(values[0]); }});
}
} // namespace coresql::blobs
