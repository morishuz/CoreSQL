#include "coresql/core.hpp"
#include <cmath>

namespace coresql {
Type integer() {
    return {"core.integer", 1, {}};
}
Type real() {
    return {"core.real", 1, {}};
}
Type text() {
    return {"core.text", 1, {}};
}
Type type_of(const Value& value) {
    if (std::holds_alternative<std::int64_t>(value))
        return integer();
    if (std::holds_alternative<double>(value))
        return real();
    if (std::holds_alternative<std::string>(value))
        return text();
    if (auto n = std::get_if<Null>(&value)) {
        if (!n->type)
            throw Error(ErrorCode::type, "Invalid NULL type");
        return *n->type;
    }
    if (auto cell = std::get_if<Compact>(&value))
        return cell->type();
    return std::get<Opaque>(value).type();
}
namespace {
template <class T> TypeAddon scalar(Type type, Layout layout) {
    TypeAddon result;
    result.id = std::move(type.id);
    result.layout = layout;
    result.native_ops = true;
    result.validate_type = [](ByteView p) {
        if (!p.empty())
            throw Error(ErrorCode::type, "Scalar type has no parameters");
    };
    result.validate_value = [](ByteView, const Value& value) {
        if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(std::get<T>(value)))
                throw Error(ErrorCode::type, "Non-finite real values are unsupported");
        }
    };
    result.equal = [](ByteView, const Value& a, const Value& b) { return std::get<T>(a) == std::get<T>(b); };
    result.compare = [](ByteView, const Value& a, const Value& b) {
        const auto& x = std::get<T>(a);
        const auto& y = std::get<T>(b);
        return (x > y) - (x < y);
    };
    result.hash = [](ByteView, const Value& value) { return std::hash<T>{}(std::get<T>(value)); };
    return result;
}
} // namespace
void install_scalar_types(Registry& registry) {
    registry.add(scalar<std::int64_t>(integer(), Layout::i64));
    registry.add(scalar<double>(real(), Layout::f64));
    registry.add(scalar<std::string>(text(), Layout::text));
}
} // namespace coresql
