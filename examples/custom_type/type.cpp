#include "type.hpp"

namespace example::codes {
using namespace coresql;
namespace {
void validate(ByteView bytes) {
    if (bytes.size() != 2 || std::any_of(bytes.begin(), bytes.end(), [](std::byte b) {
            return b < std::byte{'A'} || b > std::byte{'Z'};
        }))
        throw Error(ErrorCode::type, "CODE requires two uppercase ASCII letters");
}
} // namespace
Type type() {
    return {"example.code", 1, {}};
}
Value value(std::string_view input) {
    auto bytes = std::as_bytes(std::span(input.data(), input.size()));
    validate(bytes);
    return Opaque(type(), Bytes(bytes.begin(), bytes.end()));
}
std::string string(const Value& input) {
    auto code = std::get_if<Opaque>(&input);
    if (!code || code->type() != type())
        throw Error(ErrorCode::type, "Expected CODE value");
    auto bytes = code->bytes();
    validate(bytes);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void install(Registry& registry) {
    registry.add(EncodedTypeAddon{
        type().id, 1,
        [](ByteView parameters) {
            if (!parameters.empty())
                throw Error(ErrorCode::type, "CODE has no parameters");
        },
        [](ByteView, ByteView bytes) { validate(bytes); },
        [](ByteView, ByteView a, ByteView b) { return std::ranges::equal(a, b); },
        [](ByteView, ByteView a, ByteView b) {
            return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end())   ? -1
                   : std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end()) ? 1
                                                                                          : 0;
        }});
    registry.add(
        Function{"code.first_letter",
                 [](std::span<const Type> input) {
                     if (input.size() != 1 || input[0] != type())
                         throw Error(ErrorCode::type, "Expected one CODE");
                     return text();
                 },
                 [](std::span<const Value> input) -> Value { return string(input[0]).substr(0, 1); }});
}
} // namespace example::codes
