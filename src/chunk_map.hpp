#pragma once
#include "coresql/core.hpp"
#include <algorithm>

namespace coresql::detail {
struct Chunk;
// Dense, ID-sorted metadata. Copies allocate once; deleted IDs leave no holes.
// Inserts/erases invalidate iterators, so mutations iterate the original snapshot.
class ChunkMap {
    using Entry = std::pair<std::uint64_t, std::shared_ptr<Chunk>>;
    std::vector<Entry> entries_;
    std::size_t position(std::uint64_t id) const {
        return static_cast<std::size_t>(std::lower_bound(entries_.begin(), entries_.end(), id,
            [](const Entry& entry, std::uint64_t key) { return entry.first < key; }) - entries_.begin());
    }
public:
    auto begin() const { return entries_.begin(); }
    auto end() const { return entries_.end(); }
    auto rbegin() { return entries_.rbegin(); }
    auto rbegin() const { return entries_.rbegin(); }
    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    auto find(std::uint64_t id) const {
        auto i = position(id);
        return i < size() && entries_[i].first == id ? begin() + static_cast<std::ptrdiff_t>(i) : end();
    }
    bool contains(std::uint64_t id) const { return find(id) != end(); }
    std::shared_ptr<Chunk>& operator[](std::uint64_t id) {
        auto i = position(id);
        if (i == size() || entries_[i].first != id)
            entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i), {id, {}});
        return entries_[i].second;
    }
    void reserve_insert() { if (entries_.size() == entries_.capacity()) entries_.reserve(std::max(std::size_t{1}, entries_.size() * 2)); }
    void emplace(std::uint64_t id, std::shared_ptr<Chunk> chunk) {
        auto i = position(id);
        if (i == size() || entries_[i].first != id)
            entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i), {id, std::move(chunk)});
    }
    void remove_empty() { std::erase_if(entries_, [](const Entry& entry) { return !entry.second; }); }
    std::size_t erase(std::uint64_t id) {
        auto i = position(id);
        if (i == size() || entries_[i].first != id) return 0;
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        return 1;
    }
};
}
