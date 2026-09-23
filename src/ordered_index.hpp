#pragma once
#include "coresql/core.hpp"

namespace coresql::detail {
// Immutable AVL nodes share untouched branches across transaction snapshots.
// The final row-location tie-breaker permits duplicate non-unique keys.
class OrderedIndex {
    struct Node;
    using Link = std::shared_ptr<const Node>;
    struct Node {
        std::shared_ptr<const Row> key;
        RowLocation location;
        Link left, right;
        int height;
    };
    Link root_;
    std::vector<TypeAddon> addons_;
    std::vector<Type> types_;
    int compare(const Row&, const Row&) const;
    static int height(const Link&);
    static Link node(std::shared_ptr<const Row>, RowLocation, Link, Link);
    static Link balance(Link);
    Link insert(Link, std::shared_ptr<const Row>, RowLocation) const;
    Link erase(Link, const Row&, RowLocation) const;

public:
    IndexDefinition definition;
    std::vector<std::size_t> columns;
    OrderedIndex(IndexDefinition, const std::vector<Column>&, const Registry&);
    Row key(const Row&) const;
    std::size_t validate() const;
    void insert(const Row&, RowLocation);
    void erase(const Row&, RowLocation);
    std::vector<RowLocation> equal(const Row&) const;
    std::vector<RowLocation> range(const Value&, const Value&) const;
    // Equality prefix followed by an optional inclusive range, in logical order.
    std::vector<RowLocation> prefix_range(const Row&, const Value* lo, const Value* hi) const;
    struct Bounds {
        Row prefix;
        std::optional<Value> lower, upper;
    };
    struct Entry {
        std::shared_ptr<const Row> key;
        RowLocation location;
    };
    // Bounds and index must outlive the walk. Keys retain their immutable image.
    // In-order walk of the index. reverse yields the opposite index order.
    class Walk {
        std::vector<Link> stack_;
        bool reverse_ = false;
        const OrderedIndex* index_;
        const Bounds* bounds_;
        int position(const Row&) const;
        void descend(Link);

    public:
        Walk(const OrderedIndex&, bool reverse, const Bounds* = nullptr);
        Walk(const Walk&) = delete;
        Walk& operator=(const Walk&) = delete;
        Walk(Walk&&) noexcept = default;
        std::optional<RowLocation> next();
        std::optional<Entry> next_entry();
        std::size_t buffer_bytes() const { return stack_.capacity() * sizeof(Link); }
    };
};
} // namespace coresql::detail
