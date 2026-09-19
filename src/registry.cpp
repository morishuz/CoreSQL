#include "comparison.hpp"
#include "key_index.hpp"
#include <cstring>
#include <mutex>
#include <tuple>

namespace coresql {
namespace {
[[noreturn]] void fail(ErrorCode code, const std::string& message) { throw Error(code, message); }
const Type& intern_type(Type type) {
    using Key = std::tuple<std::string, std::uint32_t, Bytes>;
    static std::mutex lock;
    static std::map<Key, Type> table;
    std::lock_guard guard(lock);
    Key key{type.id, type.version, type.parameters};
    auto found = table.find(key);
    if (found == table.end())
        found = table.emplace(std::move(key), std::move(type)).first;
    return found->second;
}
void reject_default_identity(const Type& type) {
    if (type.id == integer().id || type.id == real().id || type.id == text().id)
        fail(ErrorCode::type, "Default identity is untagged");
}
bool holds_layout(const TypeAddon& addon, const Type& type, const Value& value) {
    switch (addon.layout) {
    case Layout::i64:
        if (type == integer())
            return std::holds_alternative<std::int64_t>(value);
        if (auto* cell = std::get_if<Compact>(&value))
            return cell->type() == type && cell->bytes().size() == 8;
        return false;
    case Layout::f64:
        return std::holds_alternative<double>(value);
    case Layout::text:
        return std::holds_alternative<std::string>(value);
    case Layout::i128:
        if (auto* cell = std::get_if<Compact>(&value))
            return cell->type() == type && cell->bytes().size() == 16;
        return false;
    case Layout::bytes:
        if (auto* opaque = std::get_if<Opaque>(&value))
            return opaque->type() == type;
        return false;
    }
    return false;
}
void validate_value(const TypeAddon& addon, const Type& type, const Value& value) {
    if (is_null(value)) { if (type_of(value) != type) fail(ErrorCode::type, "NULL type mismatch"); return; }
    if (!holds_layout(addon, type, value))
        fail(ErrorCode::type, "Value does not match column/function type: " + type.id);
    addon.validate_value(type.parameters, value);
}

}
Opaque::Opaque(Type type, Bytes bytes) : data_(std::make_shared<const Data>(Data{std::move(type), std::move(bytes)})) {}
Compact::Compact(Type type, std::int64_t payload) : width_(8) {
    reject_default_identity(type);
    type_ = &intern_type(std::move(type));
    std::memcpy(payload_.data(), &payload, 8);
}
Compact::Compact(Type type, std::array<std::byte, 16> payload) : payload_(payload), width_(16) {
    reject_default_identity(type);
    type_ = &intern_type(std::move(type));
}
Value compact(Type type, std::int64_t payload) { return Compact(std::move(type), payload); }
Value compact(Type type, std::array<std::byte, 16> payload) { return Compact(std::move(type), payload); }
std::int64_t i64_payload(const Value& value) {
    if (auto* i = std::get_if<std::int64_t>(&value))
        return *i;
    auto* cell = std::get_if<Compact>(&value);
    if (!cell || cell->bytes().size() != 8)
        fail(ErrorCode::type, "Expected i64 payload");
    std::int64_t payload = 0;
    std::memcpy(&payload, cell->bytes().data(), 8);
    return payload;
}
TypeAddon encoded_type(EncodedTypeAddon source) {
    TypeAddon result;
    result.id = std::move(source.id); result.version = source.version;
    result.validate_type = std::move(source.validate_type);
    if (source.validate_value) result.validate_value = [f = std::move(source.validate_value)](ByteView p, const Value& v) { f(p, std::get<Opaque>(v).bytes()); };
    if (source.equal) result.equal = [f = std::move(source.equal)](ByteView p, const Value& a, const Value& b) { return f(p, std::get<Opaque>(a).bytes(), std::get<Opaque>(b).bytes()); };
    if (source.compare) result.compare = [f = std::move(source.compare)](ByteView p, const Value& a, const Value& b) { return f(p, std::get<Opaque>(a).bytes(), std::get<Opaque>(b).bytes()); };
    return result;
}
Registry::Registry(bool scalar_types) {
    add(IndexAddon{"core.hash", make_hash_index, true});
    if (scalar_types) { install_scalar_types(*this); install_aggregate_functions(*this); install_relational_functions(*this); }
}
void Registry::add(TypeAddon type) {
    if (type.id.empty() || !type.version || !type.validate_type || !type.validate_value ||
        static_cast<std::size_t>(type.layout) > static_cast<std::size_t>(Layout::i128))
        fail(ErrorCode::type, "Invalid type registration");
    const bool default_identity =
        type.id == integer().id || type.id == real().id || type.id == text().id;
    if (default_identity) {
        auto expected = type.id == integer().id ? Layout::i64 : type.id == real().id ? Layout::f64 : Layout::text;
        if (type.layout != expected || type.version != 1)
            fail(ErrorCode::type, "Default identity requires its compact layout");
    } else if (type.layout == Layout::f64 || type.layout == Layout::text)
        fail(ErrorCode::type, "Compact layout requires its default identity");
    auto key = std::make_pair(type.id, type.version);
    if (!types_.emplace(std::move(key), std::move(type)).second) fail(ErrorCode::type, "Duplicate type registration");
}
void Registry::add(AggregateFunction function) {
    if (function.name.empty() || !function.infer || !function.create) fail(ErrorCode::type, "Invalid aggregate registration");
    auto name = function.name;
    if (!aggregates_.emplace(name, std::move(function)).second) fail(ErrorCode::type, "Duplicate aggregate registration");
}
const AggregateFunction& Registry::aggregate(const std::string& name) const {
    auto found = aggregates_.find(name);
    if (found == aggregates_.end()) fail(ErrorCode::type, "Unknown aggregate: " + name);
    return found->second;
}
void Registry::add(Function function) {
    if (function.name.empty() || !function.infer || !function.invoke) fail(ErrorCode::type, "Invalid function registration");
    auto name = function.name;
    if (!functions_.emplace(std::move(name), std::move(function)).second) fail(ErrorCode::type, "Duplicate function registration");
}
void Registry::add(IndexAddon index) {
    if (index.id.empty() || !index.create) fail(ErrorCode::type, "Invalid index registration");
    auto name = index.id;
    if (!indexes_.emplace(std::move(name), std::move(index)).second) fail(ErrorCode::type, "Duplicate index registration");
}
void Registry::add(KeyExtractor extractor) {
    if (extractor.name.empty() || !extractor.extract) fail(ErrorCode::type, "Invalid key extractor");
    validate(extractor.source_type); validate(extractor.key_type);
    const auto key_addon = addon(extractor.key_type);
    if (!key_addon.equal || !key_addon.hash)
        fail(ErrorCode::unsupported, "Extracted keys require equality and hashing");
    const auto name = extractor.name;
    if (extractors_.contains(name) || indexes_.contains(name)) fail(ErrorCode::type, "Duplicate extractor/index registration");
    IndexAddon index{name, [extractor, key_addon](const Type& source, const TypeAddon&) {
        if (source != extractor.source_type) fail(ErrorCode::type, "Extractor source type differs");
        return detail::make_key_index(extractor, key_addon);
    }};
    extractors_.emplace(name, std::move(extractor));
    try { add(std::move(index)); }
    catch (...) { extractors_.erase(name); throw; }
}
const KeyExtractor& Registry::extractor(const std::string& name) const {
    auto found = extractors_.find(name);
    if (found == extractors_.end()) fail(ErrorCode::type, "Missing key extractor: " + name);
    return found->second;
}
std::shared_ptr<Index> Registry::index(const std::string& name, const Type& type, bool require_unique) const {
    validate(type);
    auto found = indexes_.find(name);
    if (found == indexes_.end()) fail(ErrorCode::type, "Missing index implementation: " + name);
    if (require_unique && !found->second.unique) fail(ErrorCode::unsupported, "Primary key requires a unique index");
    auto result = found->second.create(type, addon(type));
    if (!result) fail(ErrorCode::type, "Index factory returned null");
    return result;
}
const TypeAddon& Registry::addon(const Type& type) const {
    auto found = types_.find({type.id, type.version});
    if (found == types_.end()) fail(ErrorCode::type, "Missing type/version: " + type.id + "/" + std::to_string(type.version));
    return found->second;
}
void Registry::validate(const Type& type) const { addon(type).validate_type(type.parameters); }
void Registry::validate(const Value& value, const Type& expected) const {
    const auto& extension = addon(expected);
    extension.validate_type(expected.parameters);
    validate_value(extension, expected, value);
}
bool Registry::orderable(const Type& type) const { validate(type); return bool(addon(type).compare); }
bool Registry::equatable(const Type& type) const { validate(type); return bool(addon(type).equal); }
int Registry::compare(const Value& a, const Value& b) const {
    const auto type = type_of(a); const auto& extension = addon(type);
    extension.validate_type(type.parameters);
    validate_value(extension, type, a); validate_value(extension, type, b);
    if (!extension.compare) fail(ErrorCode::unsupported, "Type does not support ordering: " + type.id);
    if (is_null(a) || is_null(b)) return int(is_null(b)) - int(is_null(a));
    return extension.compare(type.parameters, a, b);
}
bool Registry::equal(const Value& a, const Value& b) const {
    const auto type = type_of(a); const auto& extension = addon(type);
    extension.validate_type(type.parameters);
    validate_value(extension, type, a); validate_value(extension, type, b);
    if (!extension.equal) fail(ErrorCode::unsupported, "Type does not support equality: " + type.id);
    if (is_null(a) || is_null(b)) return is_null(a) && is_null(b);
    return extension.equal(type.parameters, a, b);
}
const Function& Registry::function(const std::string& name) const {
    auto found = functions_.find(name);
    if (found == functions_.end()) fail(ErrorCode::type, "Unknown function: " + name);
    return found->second;
}
detail::Comparison::Comparison(const Registry& registry, const Type& type, bool equality)
    : parameters_(type.parameters), extension_(&registry.addon(type)) {
    registry.validate(type);
    if (equality ? !extension_->equal : !extension_->compare)
        fail(ErrorCode::unsupported, "Type does not support comparison: " + type.id);
}
int detail::Comparison::compare(const Value& a, const Value& b) const { if (is_null(a) || is_null(b)) return int(is_null(b)) - int(is_null(a)); return extension_->compare(parameters_, a, b); }
bool detail::Comparison::equal(const Value& a, const Value& b) const { if (is_null(a) || is_null(b)) return is_null(a) && is_null(b); return extension_->equal(parameters_, a, b); }
} // namespace coresql
