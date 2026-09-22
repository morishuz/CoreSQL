#pragma once
#include "coresql/core.hpp"
#include <algorithm>

namespace coresql::detail {
struct Chunk;
struct Page;
class Pager;
class ChunkRef {
    std::shared_ptr<Chunk> resident_;
    std::shared_ptr<Page> page_;
    friend class Pager;
    explicit ChunkRef(std::shared_ptr<Page> page) : page_(std::move(page)) {}

public:
    ChunkRef() = default;
    ChunkRef(std::shared_ptr<Chunk> chunk) : resident_(std::move(chunk)) {}
    std::shared_ptr<Chunk> pin() const;
    // Only private, mutable transaction chunks may use this accessor.
    std::shared_ptr<Chunk>& writable();
    std::size_t rows() const;
    std::size_t payload_bytes() const;
    std::size_t encoded_bytes() const;
    bool paged() const { return bool(page_); }
    explicit operator bool() const { return bool(resident_) || bool(page_); }
    void reset() {
        resident_.reset();
        page_.reset();
    }
    bool operator==(const ChunkRef& other) const {
        return resident_ == other.resident_ && page_ == other.page_;
    }
};
// Dense, ID-sorted metadata. Copies allocate once; deleted IDs leave no holes.
// Inserts/erases invalidate iterators, so mutations iterate the original snapshot.
class ChunkMap {
    using Entry = std::pair<std::uint64_t, ChunkRef>;
    std::vector<Entry> entries_;
    std::size_t position(std::uint64_t id) const {
        return static_cast<std::size_t>(
            std::lower_bound(entries_.begin(), entries_.end(), id,
                             [](const Entry& entry, std::uint64_t key) { return entry.first < key; }) -
            entries_.begin());
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
    ChunkRef& operator[](std::uint64_t id) {
        auto i = position(id);
        if (i == size() || entries_[i].first != id)
            entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i), {id, {}});
        return entries_[i].second;
    }
    void reserve_insert() {
        if (entries_.size() == entries_.capacity())
            entries_.reserve(std::max(std::size_t{1}, entries_.size() * 2));
    }
    void emplace(std::uint64_t id, ChunkRef chunk) {
        auto i = position(id);
        if (i == size() || entries_[i].first != id)
            entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i), {id, std::move(chunk)});
    }
    void remove_empty() {
        std::erase_if(entries_, [](const Entry& entry) { return !entry.second; });
    }
    std::size_t erase(std::uint64_t id) {
        auto i = position(id);
        if (i == size() || entries_[i].first != id)
            return 0;
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        return 1;
    }
};
} // namespace coresql::detail
