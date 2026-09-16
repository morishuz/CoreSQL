#include "coresql/spatial.hpp"
#include "coresql/encoding.hpp"
#include <bit>
#include <cmath>

namespace coresql::spatial {
namespace {
void validate(Box b) {
    if (!std::isfinite(b.min_x) || !std::isfinite(b.min_y) || !std::isfinite(b.max_x) || !std::isfinite(b.max_y) || b.min_x > b.max_x || b.min_y > b.max_y)
        throw Error(ErrorCode::type, "Box requires finite, ordered bounds");
}
Box decode(ByteView bytes) {
    encoding::Reader reader(bytes);
    Box b{std::bit_cast<double>(reader.u64()), std::bit_cast<double>(reader.u64()), std::bit_cast<double>(reader.u64()), std::bit_cast<double>(reader.u64())};
    reader.end(); return b;
}
// A deliberately small alternative to an R-tree: sorted lower x bounds prune
// the upper side, then exact overlap checks filter the remaining entries.
class IntervalIndex final : public Index {
    struct Entry { Box box; RowLocation location; };
    std::multimap<double, Entry> entries;
public:
    std::shared_ptr<Index> clone() const override { return std::make_shared<IntervalIndex>(*this); }
    void insert(const Value& v, RowLocation location) override {
        auto box = bounds(v); entries.emplace(box.min_x, Entry{box, location});
    }
    void erase(const Value& v, RowLocation location) override {
        auto box = bounds(v);
        auto [first, last] = entries.equal_range(box.min_x);
        for (auto it = first; it != last; ++it) if (it->second.location == location && it->second.box == box) { entries.erase(it); return; }
        throw Error(ErrorCode::state, "Missing spatial index entry");
    }
    void validate(std::span<const IndexEntry> source) const override {
        std::map<RowLocation, Box> expected;
        for (const auto& entry : source)
            expected.emplace(entry.location, bounds(entry.value));
        for (const auto& [key, entry] : entries) {
            auto found = expected.find(entry.location);
            if (found == expected.end() || found->second != entry.box || key != entry.box.min_x)
                throw Error(ErrorCode::state, "Spatial index mismatch");
            expected.erase(found);
        }
        if (!expected.empty())
            throw Error(ErrorCode::state, "Missing spatial index entries");
    }
    void validate_search(const std::string& op) const override {
        if (op != "overlaps") throw Error(ErrorCode::unsupported, "Spatial index supports overlaps");
    }
    IndexResult search(const std::string& op, const Value& v) const override {
        validate_search(op); auto box = bounds(v);
        std::vector<RowLocation> result;
        for (auto it = entries.begin(), end = entries.upper_bound(box.max_x); it != end; ++it)
            if (it->second.box.max_x >= box.min_x) result.push_back(it->second.location);
        return {std::move(result), false};
    }
    bool matches(const std::string& op, const Value& a, const Value& b) const override {
        validate_search(op); return overlaps(bounds(a), bounds(b));
    }
};
}
Type type() { return {"coresql.box.f64", 1, {}}; }
Value value(Box box) {
    validate(box); Bytes bytes;
    for (auto v : {box.min_x, box.min_y, box.max_x, box.max_y}) encoding::u64(bytes, std::bit_cast<std::uint64_t>(v));
    return Opaque(type(), std::move(bytes));
}
Box bounds(const Value& v) {
    const auto* opaque = std::get_if<Opaque>(&v);
    if (!opaque || opaque->type() != type()) throw Error(ErrorCode::type, "Expected box");
    auto result = decode(opaque->bytes()); validate(result); return result;
}
bool overlaps(Box a, Box b) { return a.min_x <= b.max_x && a.max_x >= b.min_x && a.min_y <= b.max_y && a.max_y >= b.min_y; }
void install(Registry& registry) {
    registry.add(EncodedTypeAddon{type().id, 1,
        [](ByteView p) { if (!p.empty()) throw Error(ErrorCode::type, "Box has no parameters"); },
        [](ByteView, ByteView v) { validate(decode(v)); },
        [](ByteView, ByteView a, ByteView b) { return decode(a) == decode(b); }, {}});
    registry.add(IndexAddon{index_id, [](const Type& t, const TypeAddon&) {
        if (t != type()) throw Error(ErrorCode::type, "Spatial index requires box type");
        return std::make_shared<IntervalIndex>();
    }});
}
}
