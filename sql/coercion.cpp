#include "value_policy.hpp"

namespace coresql::sql::detail {
// SQL's dynamic value representation is intentionally limited to native scalars.
bool scalar_type(const Type& t) {
    return t == integer() || t == real() || t == text() || t == any_type();
}
bool convertible(const Type& source, const Type& target, const TypeAdapters& types) {
    return source == target || (target == any_type() && scalar_type(source)) ||
           types.convertible(source, target);
}
Value convert(Value v, const Type& target, bool explicit_cast, const TypeAdapters& types) {
    v = unpack(v);
    if (is_null(v))
        return Null(target);
    if (type_of(v) == target)
        return v;
    if (auto converted = types.try_convert(v, target, explicit_cast))
        return std::move(*converted);
    if (target == any_type())
        return pack(v);
    throw Error(ErrorCode::type, "Unsupported SQL conversion");
}
Value affinity(Value v, const Type& type, const TypeAdapters& types) {
    v = unpack(v);
    if (is_null(v))
        return v;
    auto adapter = types.find(type);
    return adapter && adapter->apply_affinity ? adapter->apply_affinity(v) : v;
}
std::string affinity_function(const Type& type, bool equality) {
    return std::string(equality ? "sql.equal_affinity." : "sql.affinity.") + type.id + "." +
           std::to_string(type.version);
}
void install_coercions(Registry& r, const TypeAdapters& adapters) {
    for (auto [name, type] :
         {std::pair{"integer", integer()}, {"real", real()}, {"text", text()}, {"box", any_type()}}) {
        auto conversion = [type, adapters](std::span<const Type> t) {
            if (t.size() != 1 || !convertible(t[0], type, adapters))
                throw Error(ErrorCode::type, "Unsupported SQL conversion");
            return type;
        };
        r.add(
            Function{"sql.cast_" + std::string(name), conversion, [type, adapters](std::span<const Value> v) {
                         return convert(v[0], type, true, adapters);
                     }});
        r.add(Function{
            "sql.store_" + std::string(name), conversion,
            [type, adapters](std::span<const Value> v) { return convert(v[0], type, false, adapters); }});
    }
    for (const auto& type : {integer(), real(), text()}) {
        r.add(Function{affinity_function(type),
                       [type](std::span<const Type> input) {
                           if (input.size() != 1 || !scalar_type(input[0]))
                               throw Error(ErrorCode::type, "Affinity requires a SQL scalar");
                           return type == text() ? text() : any_type();
                       },
                       [type, adapters](std::span<const Value> values) {
                           auto value = affinity(values[0], type, adapters);
                           return type == text() ? value : pack(value);
                       }});
    }
}
} // namespace coresql::sql::detail
