#pragma once
#include "comparison.hpp"
#include <algorithm>
#include <map>
#include <unordered_map>

namespace coresql::detail {
inline bool hashable_group(const TypeAddon& addon) {
    return addon.native_ops && addon.hash && addon.equal && addon.compare;
}

// Input keys are validated and borrowed. Native i64 groups use a payload hash;
// other native_ops keys use a row hash. Final traversal sorts completed groups
// so order matches the ordered-map path.
template <class State> class GroupTable {
    struct KeyHash {
        using is_transparent = void;
        std::vector<const TypeAddon*> addons;
        std::vector<Bytes> parameters;
        std::size_t mix(std::span<const Value> key) const {
            std::size_t hash = 0;
            for (std::size_t i = 0; i < addons.size(); ++i) {
                std::size_t part = is_null(key[i]) ? 0 : addons[i]->hash(parameters[i], key[i]) + 1;
                hash ^= part + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
        std::size_t operator()(const Row& key) const { return mix(key); }
        std::size_t operator()(std::span<const Value> key) const { return mix(key); }
    };
    struct KeyEqual {
        using is_transparent = void;
        std::vector<const TypeAddon*> addons;
        std::vector<Bytes> parameters;
        bool same(std::span<const Value> a, std::span<const Value> b) const {
            for (std::size_t i = 0; i < addons.size(); ++i) {
                if (is_null(a[i]) || is_null(b[i])) {
                    if (is_null(a[i]) != is_null(b[i]))
                        return false;
                    continue;
                }
                if (!addons[i]->equal(parameters[i], a[i], b[i]))
                    return false;
            }
            return true;
        }
        bool operator()(const Row& a, const Row& b) const { return same(a, b); }
        bool operator()(const Row& a, std::span<const Value> b) const { return same(a, b); }
        bool operator()(std::span<const Value> a, const Row& b) const { return same(a, b); }
    };

    QueryRowLess less_;
    bool native_ = false;
    bool hashed_ = false;
    std::map<Row, State, QueryRowLess> ordered_;
    std::unordered_map<std::int64_t, std::size_t> integers_;
    std::optional<std::size_t> null_;
    std::vector<std::pair<Row, State>> entries_;
    std::unordered_map<Row, std::size_t, KeyHash, KeyEqual> hashed_map_;

public:
    GroupTable(const Registry& registry, std::span<const Type> types, bool repeatable)
        : less_(registry, types, repeatable),
          native_(repeatable && types.size() == 1 && native_i64(registry.addon(types[0]))),
          hashed_(!native_ && repeatable &&
                  std::all_of(types.begin(), types.end(),
                              [&](const Type& t) { return hashable_group(registry.addon(t)); })),
          ordered_(less_), hashed_map_(0, hasher(registry, types), equaler(registry, types)) {}

    template <class Create> State& get(std::span<const Value> key, Create create) {
        if (native_) {
            const bool missing = is_null(key[0]);
            if (missing) {
                if (null_)
                    return entries_[*null_].second;
            } else if (auto found = integers_.find(i64_payload(key[0])); found != integers_.end())
                return entries_[found->second].second;
            const auto index = entries_.size();
            entries_.emplace_back(Row(key.begin(), key.end()), create());
            if (missing)
                null_ = index;
            else
                integers_.emplace(i64_payload(key[0]), index);
            return entries_.back().second;
        }
        if (hashed_) {
            if (auto found = hashed_map_.find(key); found != hashed_map_.end())
                return entries_[found->second].second;
            const auto index = entries_.size();
            entries_.emplace_back(Row(key.begin(), key.end()), create());
            hashed_map_.emplace(entries_.back().first, index);
            return entries_.back().second;
        }
        auto found = ordered_.find(key);
        if (found == ordered_.end())
            found = ordered_.emplace(Row(key.begin(), key.end()), create()).first;
        return found->second;
    }

    template <class Visit> void finish(Visit visit) {
        if (native_ || hashed_) {
            std::sort(entries_.begin(), entries_.end(),
                      [&](const auto& a, const auto& b) { return less_(a.first, b.first); });
            for (auto& [key, state] : entries_)
                visit(key, state);
        } else
            for (auto& [key, state] : ordered_)
                visit(key, state);
    }

private:
    static KeyHash hasher(const Registry& registry, std::span<const Type> types) {
        KeyHash hash;
        for (const auto& type : types) {
            hash.addons.push_back(&registry.addon(type));
            hash.parameters.push_back(type.parameters);
        }
        return hash;
    }
    static KeyEqual equaler(const Registry& registry, std::span<const Type> types) {
        KeyEqual equal;
        for (const auto& type : types) {
            equal.addons.push_back(&registry.addon(type));
            equal.parameters.push_back(type.parameters);
        }
        return equal;
    }
};
} // namespace coresql::detail
