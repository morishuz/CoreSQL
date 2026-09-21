#include "ordered_index.hpp"
#include "query_control.hpp"
#include <algorithm>
#include <set>

namespace coresql::detail {
OrderedIndex::OrderedIndex(IndexDefinition d, const std::vector<Column>& schema, const Registry& registry)
    : definition(std::move(d)) {
    if (definition.name.empty() || definition.columns.empty())
        throw Error(ErrorCode::schema, "Index needs a name and columns");
    std::set<std::string> seen;
    for (const auto& name : definition.columns) {
        auto c =
            std::find_if(schema.begin(), schema.end(), [&](const auto& col) { return col.name == name; });
        if (c == schema.end() || !seen.insert(name).second)
            throw Error(ErrorCode::schema, "Invalid index column");
        if (!registry.orderable(c->type))
            throw Error(ErrorCode::type, "Ordered index needs ordering");
        columns.push_back(static_cast<std::size_t>(c - schema.begin()));
        types_.push_back(c->type);
        addons_.push_back(registry.addon(c->type));
    }
    if (!definition.descending.empty() && definition.descending.size() != columns.size())
        throw Error(ErrorCode::schema, "Index direction count mismatch");
}
int OrderedIndex::compare(const Row& a, const Row& b) const {
    for (std::size_t i = 0; i < a.size(); ++i) {
        int c = (is_null(a[i]) || is_null(b[i])) ? int(is_null(b[i])) - int(is_null(a[i]))
                                                 : addons_[i].compare(types_[i].parameters, a[i], b[i]);
        c = (c > 0) - (c < 0);
        if (c)
            return definition.descending.empty() || !definition.descending[i] ? c : -c;
    }
    return 0;
}
int OrderedIndex::height(const Link& n) {
    return n ? n->height : 0;
}
OrderedIndex::Link OrderedIndex::node(std::shared_ptr<const Row> k, RowLocation p, Link l, Link r) {
    int h = 1 + std::max(height(l), height(r));
    return std::make_shared<Node>(Node{std::move(k), p, std::move(l), std::move(r), h});
}
OrderedIndex::Link OrderedIndex::balance(Link n) {
    const int skew = height(n->left) - height(n->right);
    if (skew > 1) {
        auto l = n->left;
        if (height(l->left) < height(l->right)) {
            auto m = l->right;
            l = node(m->key, m->location, node(l->key, l->location, l->left, m->left), m->right);
        }
        return node(l->key, l->location, l->left, node(n->key, n->location, l->right, n->right));
    }
    if (skew < -1) {
        auto r = n->right;
        if (height(r->right) < height(r->left)) {
            auto m = r->left;
            r = node(m->key, m->location, m->left, node(r->key, r->location, m->right, r->right));
        }
        return node(r->key, r->location, node(n->key, n->location, n->left, r->left), r->right);
    }
    return n;
}
OrderedIndex::Link OrderedIndex::insert(Link n, std::shared_ptr<const Row> k, RowLocation p) const {
    if (!n)
        return node(k, p, {}, {});
    int c = compare(*k, *n->key);
    if (!c && definition.unique && !std::any_of(k->begin(), k->end(), is_null))
        throw Error(ErrorCode::constraint, "Duplicate unique index key");
    if (!c)
        c = (p > n->location) - (p < n->location);
    if (!c)
        throw Error(ErrorCode::state, "Duplicate index row location");
    return balance(c < 0 ? node(n->key, n->location, insert(n->left, k, p), n->right)
                         : node(n->key, n->location, n->left, insert(n->right, k, p)));
}
OrderedIndex::Link OrderedIndex::erase(Link n, const Row& k, RowLocation p) const {
    if (!n)
        throw Error(ErrorCode::state, "Missing ordered index entry");
    int c = compare(k, *n->key);
    if (!c)
        c = (p > n->location) - (p < n->location);
    if (c < 0)
        return balance(node(n->key, n->location, erase(n->left, k, p), n->right));
    if (c > 0)
        return balance(node(n->key, n->location, n->left, erase(n->right, k, p)));
    if (!n->left)
        return n->right;
    if (!n->right)
        return n->left;
    auto next = n->right;
    while (next->left)
        next = next->left;
    return balance(node(next->key, next->location, n->left, erase(n->right, *next->key, next->location)));
}
Row OrderedIndex::key(const Row& row) const {
    Row k;
    for (auto c : columns)
        k.push_back(row[c]);
    return k;
}
void OrderedIndex::insert(const Row& row, RowLocation p) {
    root_ = insert(root_, std::make_shared<const Row>(key(row)), p);
}
void OrderedIndex::erase(const Row& row, RowLocation p) {
    root_ = erase(root_, key(row), p);
}
std::size_t OrderedIndex::validate() const {
    auto less = [&](const Node& a, const Node& b) {
        int c = compare(*a.key, *b.key);
        return c ? c < 0 : a.location < b.location;
    };
    auto walk = [&](auto&& self, const Link& n, const Node* lo, const Node* hi) -> std::size_t {
        if (!n)
            return 0;
        if (n->key->size() != columns.size() || (lo && !less(*lo, *n)) || (hi && !less(*n, *hi)) ||
            n->height != 1 + std::max(height(n->left), height(n->right)) ||
            std::abs(height(n->left) - height(n->right)) > 1)
            throw Error(ErrorCode::state, "Invalid ordered index structure");
        return 1 + self(self, n->left, lo, n.get()) + self(self, n->right, n.get(), hi);
    };
    return walk(walk, root_, nullptr, nullptr);
}
std::vector<RowLocation> OrderedIndex::equal(const Row& k) const {
    if (k.size() != columns.size())
        throw Error(ErrorCode::schema, "Index key width mismatch");
    std::vector<RowLocation> rows;
    auto walk = [&](auto&& self, const Link& n) -> void {
        if (!n)
            return;
        int c = compare(k, *n->key);
        if (c <= 0)
            self(self, n->left);
        if (!c)
            rows.push_back(n->location);
        if (c >= 0)
            self(self, n->right);
    };
    walk(walk, root_);
    return rows;
}
std::vector<RowLocation> OrderedIndex::range(const Value& lo, const Value& hi) const {
    // Inclusive range on the leading column, irrespective of physical direction.
    std::vector<RowLocation> rows;
    auto cmp = [&](const Value& a, const Value& b) {
        return (is_null(a) || is_null(b)) ? int(is_null(b)) - int(is_null(a))
                                          : addons_[0].compare(types_[0].parameters, a, b);
    };
    bool desc = !definition.descending.empty() && definition.descending[0];
    auto walk = [&](auto&& self, const Link& n) -> void {
        if (!n)
            return;
        int lower = cmp((*n->key)[0], lo), upper = cmp((*n->key)[0], hi);
        if (desc ? upper <= 0 : lower >= 0)
            self(self, n->left);
        if (lower >= 0 && upper <= 0)
            rows.push_back(n->location);
        if (desc ? lower >= 0 : upper <= 0)
            self(self, n->right);
    };
    walk(walk, root_);
    return rows;
}
} // namespace coresql::detail

namespace coresql::detail {
std::vector<RowLocation> OrderedIndex::prefix_range(const Row& prefix, const Value* lo,
                                                    const Value* hi) const {
    if (prefix.size() > columns.size() || ((lo || hi) && prefix.size() == columns.size()))
        throw Error(ErrorCode::schema, "Invalid index prefix width");
    std::vector<RowLocation> rows;
    auto compare_value = [&](std::size_t i, const Value& a, const Value& b) {
        return (is_null(a) || is_null(b)) ? int(is_null(b)) - int(is_null(a))
                                          : addons_[i].compare(types_[i].parameters, a, b);
    };
    auto walk = [&](auto&& self, const Link& n) -> void {
        if (!n)
            return;
        execution::query_step();
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            const int raw = compare_value(i, (*n->key)[i], prefix[i]);
            int c = (raw > 0) - (raw < 0);
            if (!definition.descending.empty() && definition.descending[i])
                c = -c;
            if (c) {
                self(self, c < 0 ? n->right : n->left);
                return;
            }
        }
        const auto i = prefix.size();
        int lower = lo ? compare_value(i, (*n->key)[i], *lo) : 1;
        int upper = hi ? compare_value(i, (*n->key)[i], *hi) : -1;
        bool desc = i < columns.size() && !definition.descending.empty() && definition.descending[i];
        if (desc ? upper <= 0 : lower >= 0)
            self(self, n->left);
        if (lower >= 0 && upper <= 0)
            rows.push_back(n->location);
        if (desc ? lower >= 0 : upper <= 0)
            self(self, n->right);
    };
    walk(walk, root_);
    return rows;
}
} // namespace coresql::detail
