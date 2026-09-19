#include "coresql/core.hpp"
#include <array>
#include <unordered_map>

namespace coresql {
namespace {
struct HashIndex final : Index {
    struct Less {
        std::shared_ptr<const TypeAddon> addon;
        Bytes parameters;
        bool operator()(const Value& a, const Value& b) const { return addon->compare(parameters, a, b) < 0; }
    };
    using Bucket = std::map<Value, RowLocation, Less>;
    using IntBucket = std::unordered_map<std::int64_t, RowLocation>;
    Less less;
    bool native = false;
    std::array<std::shared_ptr<Bucket>, 256> buckets{};
    std::array<std::shared_ptr<IntBucket>, 256> integers{};
    HashIndex(const Type& type, const TypeAddon& addon)
        : less{std::make_shared<TypeAddon>(addon), type.parameters}, native(native_i64(addon)) {
        if (!addon.equal || !addon.hash || (!native && !addon.compare))
            throw Error(ErrorCode::unsupported, "Hash keys require equality, consistent ordering and hashing");
    }
    std::size_t number(const Value& v) const {
        if (native)
            return static_cast<std::uint64_t>(i64_payload(v)) % integers.size();
        return less.addon->hash(less.parameters, v) % buckets.size();
    }
    Bucket& writable(std::size_t n) {
        auto& bucket = buckets[n];
        if (!bucket) bucket = std::make_shared<Bucket>(less);
        else if (bucket.use_count() != 1) bucket = std::make_shared<Bucket>(*bucket);
        return *bucket;
    }
    IntBucket& writable_int(std::size_t n) {
        auto& bucket = integers[n];
        if (!bucket)
            bucket = std::make_shared<IntBucket>();
        else if (bucket.use_count() != 1)
            bucket = std::make_shared<IntBucket>(*bucket);
        return *bucket;
    }
    std::shared_ptr<Index> clone() const override { return std::make_shared<HashIndex>(*this); }
    void insert(const Value& value, RowLocation location) override {
        if (native) {
            if (!writable_int(number(value)).emplace(i64_payload(value), location).second)
                throw Error(ErrorCode::constraint, "Duplicate primary key");
            return;
        }
        if (!writable(number(value)).emplace(value, location).second)
            throw Error(ErrorCode::constraint, "Duplicate primary key");
    }
    void erase(const Value& value, RowLocation location) override {
        if (lookup(value) != location)
            throw Error(ErrorCode::state, "Missing or mismatched primary index entry");
        if (native)
            writable_int(number(value)).erase(i64_payload(value));
        else
            writable(number(value)).erase(value);
    }
    void validate(std::span<const IndexEntry> source) const override {
        std::size_t count = 0;
        if (native) {
            for (const auto& bucket : integers)
                if (bucket)
                    count += bucket->size();
        } else {
            for (const auto& bucket : buckets)
                if (bucket)
                    count += bucket->size();
        }
        if (count != source.size())
            throw Error(ErrorCode::state, "Primary index cardinality mismatch");
        for (const auto& entry : source)
            if (lookup(entry.value) != entry.location)
                throw Error(ErrorCode::state, "Primary index mismatch");
    }
    std::optional<RowLocation> lookup(const Value& value) const override {
        auto n = number(value);
        if (native) {
            const auto& bucket = integers[n];
            if (!bucket)
                return {};
            const auto found = bucket->find(i64_payload(value));
            return found == bucket->end() ? std::nullopt : std::optional(found->second);
        }
        const auto& bucket = buckets[n];
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
