#pragma once
#include "coresql/core.hpp"
#include <unordered_set>

namespace coresql::sql::detail {
// Affinity must leave the bound type unchanged. Other types/comparison providers
// retain the scalar equality path, including their error and callback behavior.
inline auto membership_factory(Registry registry, std::optional<Type> affinity = {}) {
    return [registry = std::move(registry), affinity = std::move(affinity)](
               const Type& type, std::span<const Value> candidates) -> std::function<bool(const Value&)> {
        const auto& addon = registry.addon(type);
        if ((addon.layout != Layout::i64 && addon.layout != Layout::text) || (affinity && type != *affinity))
            return {};
        if (!addon.native_ops || !registry.addon(integer()).native_ops || !addon.hash || !addon.equal ||
            !addon.compare)
            return {};
        auto hash = [f = addon.hash](const Value& v) { return f({}, v); };
        auto equal = [f = addon.equal](const Value& a, const Value& b) { return f({}, a, b); };
        if (candidates.size() <= 8) {
            return
                [keys = std::vector<Value>(candidates.begin(), candidates.end()), equal](const Value& value) {
                    return std::any_of(keys.begin(), keys.end(),
                                       [&](const Value& key) { return equal(value, key); });
                };
        }
        using Set = std::unordered_set<Value, decltype(hash), decltype(equal)>;
        auto keys = std::make_shared<Set>(0, hash, equal);
        keys->reserve(candidates.size());
        keys->insert(candidates.begin(), candidates.end());
        return [keys = std::move(keys)](const Value& value) { return keys->contains(value); };
    };
}
} // namespace coresql::sql::detail
