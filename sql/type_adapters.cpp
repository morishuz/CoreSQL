#include "coresql/sql_types.hpp"
#include "value_policy.hpp"
#include <set>

namespace coresql::sql {
namespace {
std::string name(std::string_view input) {
    std::string out(input);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c + ('a' - 'A'));
    return out;
}
[[noreturn]] void ambiguous() {
    throw Error(ErrorCode::type, "Ambiguous SQL type adapter rule");
}
} // namespace
TypeAdapters::TypeAdapters(bool scalar_defaults)
    : adapters_(std::make_shared<const std::vector<SqlTypeAdapter>>()) {
    if (scalar_defaults) {
        add(integer_adapter());
        add(real_adapter());
        add(text_adapter());
    }
}
void TypeAdapters::add(SqlTypeAdapter adapter) {
    if (adapter.type_id.empty() || !adapter.version || adapter.names.empty() || !adapter.declaration ||
        !adapter.install || !adapter.can_convert || !adapter.convert)
        throw Error(ErrorCode::type, "Incomplete SQL type adapter");
    if (find(Type{adapter.type_id, adapter.version, {}}))
        throw Error(ErrorCode::type, "SQL type adapter already registered");
    std::set<std::string> names;
    for (auto& n : adapter.names) {
        n = name(n);
        if (n.empty() || named(n) || !names.insert(n).second || n == "numeric")
            throw Error(ErrorCode::type, "Duplicate or reserved SQL type name");
    }
    auto next = std::make_shared<std::vector<SqlTypeAdapter>>(*adapters_);
    next->push_back(std::move(adapter));
    adapters_ = std::move(next);
}
const SqlTypeAdapter* TypeAdapters::named(std::string_view n) const {
    const auto key = name(n);
    for (const auto& a : *adapters_)
        if (std::find(a.names.begin(), a.names.end(), key) != a.names.end())
            return &a;
    return nullptr;
}
const SqlTypeAdapter* TypeAdapters::find(const Type& t) const {
    for (const auto& a : *adapters_)
        if (a.type_id == t.id && a.version == t.version)
            return &a;
    return nullptr;
}
void TypeAdapters::install(Registry& r) const {
    for (const auto& a : *adapters_)
        a.install(r);
}
const SqlTypeAdapter* TypeAdapters::conversion(const Type& source, const Type& target) const {
    const SqlTypeAdapter* selected = nullptr;
    for (const auto& a : *adapters_)
        if (a.can_convert(source, target)) {
            if (selected)
                ambiguous();
            selected = &a;
        }
    return selected;
}
bool TypeAdapters::convertible(const Type& s, const Type& t) const {
    return conversion(s, t) != nullptr;
}
Value TypeAdapters::convert(const Value& v, const Type& t, bool cast) const {
    auto value = try_convert(v, t, cast);
    if (!value)
        throw Error(ErrorCode::type, "Unsupported SQL extension conversion");
    return std::move(*value);
}
std::optional<Value> TypeAdapters::try_convert(const Value& v, const Type& t, bool cast) const {
    auto a = conversion(type_of(v), t);
    if (!a)
        return {};
    auto value = a->convert(v, t, cast);
    if (type_of(value) != t)
        throw Error(ErrorCode::type, "SQL adapter returned the wrong conversion type");
    return value;
}
std::optional<SqlOperation> TypeAdapters::operation(std::string_view op, std::span<const Type> types) const {
    std::optional<SqlOperation> result;
    for (const auto& a : *adapters_)
        if (a.operation)
            if (auto candidate = a.operation(op, types)) {
                if (result)
                    ambiguous();
                result = std::move(candidate);
            }
    return result ? result : detail::scalar_operation(op, types);
}
std::optional<Type> TypeAdapters::common_type(std::span<const Type> types) const {
    std::optional<Type> result;
    for (const auto& a : *adapters_)
        if (a.common_type)
            if (auto candidate = a.common_type(types)) {
                if (result)
                    ambiguous();
                result = std::move(candidate);
            }
    return result ? result : detail::scalar_common_type(types);
}
bool TypeAdapters::same_configuration(const TypeAdapters& other) const {
    return adapters_ == other.adapters_;
}
const TypeAdapters& default_type_adapters() {
    static const TypeAdapters adapters = [] {
        TypeAdapters a;
        a.add(date_adapter());
        a.add(decimal_adapter());
        a.add(vector_adapter());
        return a;
    }();
    return adapters;
}
} // namespace coresql::sql
