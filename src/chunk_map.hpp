#pragma once
#include "coresql/core.hpp"
#include <algorithm>
#include <iterator>

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
// ID-partitioned copy-on-write metadata. A table copy shares blocks; mutable
// access detaches its block before exposing a ChunkRef, preserving chunk ownership.
class ChunkMap {
    using Entry = std::pair<std::uint64_t, ChunkRef>;
    static constexpr std::size_t block_size = 64;
    struct Block {
        std::uint64_t number;
        std::vector<Entry> entries;
        explicit Block(std::uint64_t n) : number(n) { entries.reserve(block_size); }
        Block(const Block& other) : Block(other.number) {
            entries.assign(other.entries.begin(), other.entries.end());
        }
    };
    std::vector<std::shared_ptr<Block>> blocks_;
    std::size_t size_ = 0;
    std::size_t position(std::uint64_t id) const {
        const auto number = id / block_size;
        return static_cast<std::size_t>(
            std::lower_bound(blocks_.begin(), blocks_.end(), number,
                             [](const auto& block, auto n) { return block->number < n; }) -
            blocks_.begin());
    }
    static auto entry_position(const Block& block, std::uint64_t id) {
        return std::lower_bound(block.entries.begin(), block.entries.end(), id,
                                [](const Entry& entry, auto key) { return entry.first < key; });
    }
    Block& writable(std::size_t i) {
        if (blocks_[i].use_count() != 1)
            blocks_[i] = std::make_shared<Block>(*blocks_[i]);
        return *blocks_[i];
    }

public:
    class const_iterator {
        const ChunkMap* owner_ = nullptr;
        std::size_t block_ = 0, entry_ = 0;
        friend class ChunkMap;
        const_iterator(const ChunkMap* owner, std::size_t block, std::size_t entry = 0)
            : owner_(owner), block_(block), entry_(entry) {
            while (block_ < owner_->blocks_.size() && owner_->blocks_[block_]->entries.empty())
                ++block_;
        }

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = Entry;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entry*;
        using reference = const Entry&;
        const_iterator() = default;
        reference operator*() const { return owner_->blocks_[block_]->entries[entry_]; }
        pointer operator->() const { return &**this; }
        const_iterator& operator++() {
            if (++entry_ == owner_->blocks_[block_]->entries.size()) {
                entry_ = 0;
                do {
                    ++block_;
                } while (block_ < owner_->blocks_.size() && owner_->blocks_[block_]->entries.empty());
            }
            return *this;
        }
        const_iterator operator++(int) {
            auto old = *this;
            ++*this;
            return old;
        }
        const_iterator& operator--() {
            if (entry_ == 0) {
                do {
                    --block_;
                } while (owner_->blocks_[block_]->entries.empty());
                entry_ = owner_->blocks_[block_]->entries.size();
            }
            --entry_;
            return *this;
        }
        const_iterator operator--(int) {
            auto old = *this;
            --*this;
            return old;
        }
        bool operator==(const const_iterator&) const = default;
    };
    auto begin() const { return const_iterator(this, 0); }
    auto end() const { return const_iterator(this, blocks_.size()); }
    auto rbegin() const { return std::make_reverse_iterator(end()); }
    bool empty() const { return size_ == 0; }
    std::size_t size() const { return size_; }
    auto find(std::uint64_t id) const {
        auto b = position(id);
        if (b == blocks_.size() || blocks_[b]->number != id / block_size)
            return end();
        const auto& entries = blocks_[b]->entries;
        auto it = entry_position(*blocks_[b], id);
        return it != entries.end() && it->first == id
                   ? const_iterator(this, b, static_cast<std::size_t>(it - entries.begin()))
                   : end();
    }
    bool contains(std::uint64_t id) const { return find(id) != end(); }
    // Preallocate the directory and target block before mutating indexes. A
    // subsequent insertion of this ID does not allocate while ownership is held.
    void reserve_insert(std::uint64_t id) {
        auto b = position(id);
        if (b == blocks_.size() || blocks_[b]->number != id / block_size) {
            auto block = std::make_shared<Block>(id / block_size);
            blocks_.insert(blocks_.begin() + static_cast<std::ptrdiff_t>(b), std::move(block));
        } else {
            (void)writable(b);
        }
    }
    ChunkRef& operator[](std::uint64_t id) {
        reserve_insert(id);
        auto& block = *blocks_[position(id)];
        auto it = entry_position(block, id);
        const auto i = static_cast<std::size_t>(it - block.entries.cbegin());
        if (it == block.entries.end() || it->first != id) {
            block.entries.insert(it, {id, {}});
            ++size_;
        }
        return block.entries[i].second;
    }
    void emplace(std::uint64_t id, ChunkRef chunk) {
        if (!contains(id))
            (*this)[id] = std::move(chunk);
    }
    void remove_empty() {
        for (std::size_t b = 0; b < blocks_.size(); ++b) {
            const auto& entries = blocks_[b]->entries;
            if (std::any_of(entries.begin(), entries.end(), [](const auto& e) { return !e.second; })) {
                auto& block = writable(b);
                size_ -= std::erase_if(block.entries, [](const auto& e) { return !e.second; });
            }
        }
        std::erase_if(blocks_, [](const auto& b) { return b->entries.empty(); });
    }
    std::size_t erase(std::uint64_t id) {
        if (!contains(id))
            return 0;
        auto b = position(id);
        auto& block = writable(b);
        block.entries.erase(entry_position(block, id));
        --size_;
        if (block.entries.empty())
            blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(b));
        return 1;
    }
};
} // namespace coresql::detail
