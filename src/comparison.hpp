#pragma once
#include "coresql/core.hpp"

namespace coresql::detail {
bool holds_layout(const TypeAddon&, const Type&, const Value&);

// Lexicographic ordering of equal-width rows with matching column types.
// Uses the public validating comparison contract, including NULL ordering.
// The registry must outlive containers using this comparator.
struct RowLess {
    const Registry* registry;
    bool operator()(std::span<const Value> a, std::span<const Value> b) const {
        for (std::size_t i = 0; i < a.size(); ++i) {
            auto c = registry->compare(a[i], b[i]);
            if (c)
                return c < 0;
        }
        return false;
    }
};

// Query-only comparison: operands must have the same bound type and already
// be validated (stored rows, bound literals, or checked function results).
struct Comparison {
    Comparison(const Registry&, const Type&, bool equality);
    int compare(const Value&, const Value&) const;
    bool equal(const Value&, const Value&) const;

private:
    Bytes parameters_;
    const TypeAddon* extension_;
};
// Bound row ordering for repeatable queries over already validated, equal-width
// rows. Nonrepeatable queries retain public validation and callback ordering.
// The registry must outlive containers using this comparator.
struct QueryRowLess {
    using is_transparent = void;
    QueryRowLess(const Registry& registry, std::span<const Type> types, bool repeatable)
        : registry_(&registry), prepared_(repeatable) {
        if (prepared_)
            for (const auto& type : types)
                columns_.emplace_back(registry, type, false);
    }
    bool operator()(std::span<const Value> a, std::span<const Value> b) const {
        if (!prepared_)
            return RowLess{registry_}(a, b);
        for (std::size_t i = 0; i < columns_.size(); ++i) {
            auto c = columns_[i].compare(a[i], b[i]);
            if (c)
                return c < 0;
        }
        return false;
    }

private:
    const Registry* registry_;
    bool prepared_;
    std::vector<Comparison> columns_;
};
} // namespace coresql::detail
