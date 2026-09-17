#include "coresql/core.hpp"
#include <array>

namespace coresql {
namespace {
struct HashIndex final : Index {
    struct Less {
        std::shared_ptr<const TypeAddon> addon;
        Bytes parameters;
        bool operator()(const Value& a, const Value& b) const { return addon->compare(parameters, a, b) < 0; }
    };
    using Bucket = std::map<Value, RowLocation, Less>;
    Less less;
    std::array<std::shared_ptr<Bucket>, 256> buckets{};
    HashIndex(const Type& type, const TypeAddon& addon) : less{std::make_shared<TypeAddon>(addon), type.parameters} {
        if (!addon.equal || !addon.compare || !addon.hash)
            throw Error(ErrorCode::unsupported, "Hash keys require equality, consistent ordering and hashing");
    }
    std::size_t number(const Value& v) const { return less.addon->hash(less.parameters, v) % buckets.size(); }
    Bucket& writable(std::size_t n) {
        auto& bucket = buckets[n];
        if (!bucket) bucket = std::make_shared<Bucket>(less);
        else if (bucket.use_count() != 1) bucket = std::make_shared<Bucket>(*bucket);
        return *bucket;
    }
    std::shared_ptr<Index> clone() const override { return std::make_shared<HashIndex>(*this); }
    void insert(const Value& value, RowLocation location) override {
        if (!writable(number(value)).emplace(value, location).second)
            throw Error(ErrorCode::constraint, "Duplicate primary key");
    }
    void erase(const Value& value, RowLocation location) override {
        if (lookup(value) != location)
            throw Error(ErrorCode::state, "Missing or mismatched primary index entry");
        writable(number(value)).erase(value);
    }
    void validate(std::span<const IndexEntry> source) const override {
        std::size_t count = 0;
        for (const auto& bucket : buckets)
            if (bucket)
                count += bucket->size();
        if (count != source.size())
            throw Error(ErrorCode::state, "Primary index cardinality mismatch");
        for (const auto& entry : source)
            if (lookup(entry.value) != entry.location)
                throw Error(ErrorCode::state, "Primary index mismatch");
    }
    std::optional<RowLocation> lookup(const Value& value) const override {
        const auto& bucket = buckets[number(value)];
        if (!bucket) return {};
        const auto found = bucket->find(value);
        return found == bucket->end() ? std::nullopt : std::optional(found->second);
    }
};
}
std::shared_ptr<Index> make_hash_index(const Type& type, const TypeAddon& addon) { return std::make_shared<HashIndex>(type, addon); }
std::optional<RowLocation> Index::lookup(const Value&) const { throw Error(ErrorCode::unsupported, "Index has no unique lookup"); }
void Index::validate_search(const std::string&) const { throw Error(ErrorCode::unsupported, "Unsupported index search"); }
IndexResult Index::search(const std::string&, const Value&) const { throw Error(ErrorCode::unsupported, "Unsupported index search"); }
bool Index::matches(const std::string&, const Value&, const Value&) const { throw Error(ErrorCode::unsupported, "Unsupported index predicate"); }
}

namespace coresql {
void Index::validate(std::span<const IndexEntry>) const {
    throw Error(ErrorCode::unsupported, "Index does not support integrity validation");
}
Type Index::search_type(const std::string& operation, const Type& source) const { validate_search(operation); return source; }
}
