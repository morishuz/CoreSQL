#pragma once
#include "comparison.hpp"
#include <algorithm>
#include <map>
#include <unordered_map>

namespace coresql::detail {
// Input keys are validated and borrowed. Native i64 groups use a hash index;
// final traversal retains ordered grouping's key and aggregate-finish order.
template <class State> class GroupTable {
    QueryRowLess less_;
    bool native_;
    std::map<Row, State, QueryRowLess> ordered_;
    std::unordered_map<std::int64_t, std::size_t> integers_;
    std::optional<std::size_t> null_;
    std::vector<std::pair<Row, State>> entries_;

public:
    GroupTable(const Registry& registry, std::span<const Type> types, bool repeatable)
        : less_(registry, types, repeatable),
          native_(repeatable && types.size() == 1 && native_i64(registry.addon(types[0]))), ordered_(less_) {}

    template <class Create> State& get(std::span<const Value> key, Create create) {
        if (!native_) {
            auto found = ordered_.find(key);
            if (found == ordered_.end())
                found = ordered_.emplace(Row(key.begin(), key.end()), create()).first;
            return found->second;
        }
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

    template <class Visit> void finish(Visit visit) {
        if (native_) {
            std::sort(entries_.begin(), entries_.end(),
                      [&](const auto& a, const auto& b) { return less_(a.first, b.first); });
            for (auto& [key, state] : entries_)
                visit(key, state);
        } else
            for (auto& [key, state] : ordered_)
                visit(key, state);
    }
};
} // namespace coresql::detail
