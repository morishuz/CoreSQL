#pragma once
#include "coresql/core.hpp"

namespace coresql::detail {
// Shared semantics for scans and index maintenance; outputs cross a trust boundary.
struct KeyBinding {
    KeyExtractor extractor;
    TypeAddon addon;
    std::vector<Value> keys(const Value&) const;
    bool matches(const Value& source, const Value& key) const;
};
std::shared_ptr<Index> make_key_index(const KeyExtractor&, const TypeAddon&);
}
