#include "coresql/core.hpp"
#include <array>
#include <algorithm>

namespace coresql {
namespace {
struct HashIndex final : Index {
    struct Less {
        std::shared_ptr<const TypeAddon> addon;
        Bytes parameters;
        bool operator()(const Value& a, const Value& b) const { return addon->compare(parameters, a, b) < 0; }
    };
    using Bucket = std::map<Value, RowLocation, Less>;
    // Hash partitioning bounds point searches and snapshot copies. Sorted,
    // contiguous native payloads also support ranges without a second index.
    using IntBucket = std::vector<std::pair<std::int64_t, RowLocation>>;
    Less less;
    bool native = false;
    std::array<std::shared_ptr<Bucket>, 256> buckets{};
    std::array<std::shared_ptr<IntBucket>, 256> integers{};
    HashIndex(const Type& type, const TypeAddon& addon)
        : less{std::make_shared<TypeAddon>(addon), type.parameters}, native(native_i64(addon)) {
        if (!addon.equal || !addon.hash || (!native && !addon.compare))
            throw Error(ErrorCode::unsupported,
                        "Hash keys require equality, consistent ordering and hashing");
    }
    std::size_t number(const Value& v) const {
        if (native)
            return static_cast<std::uint64_t>(i64_payload(v)) % integers.size();
        return less.addon->hash(less.parameters, v) % buckets.size();
    }
    Bucket& writable(std::size_t n) {
        auto& bucket = buckets[n];
        if (!bucket)
            bucket = std::make_shared<Bucket>(less);
        else if (bucket.use_count() != 1)
            bucket = std::make_shared<Bucket>(*bucket);
        return *bucket;
    }
    IntBucket& writable_int(std::size_t n) {
        auto& bucket = integers[n];
        if (!bucket)
            bucket = std::make_shared<IntBucket>();
        else if (bucket.use_count() != 1) {
            auto copy = std::make_shared<IntBucket>();
            // Reuse a small set of allocation sizes during steady growth.
            copy->reserve(bucket->size() + 64 - bucket->size() % 64);
            copy->insert(copy->end(), bucket->begin(), bucket->end());
            bucket = std::move(copy);
        }
        return *bucket;
    }
    template <class BucketType> static auto lower(BucketType& bucket, std::int64_t key) {
        return std::lower_bound(bucket.begin(), bucket.end(), key,
                                [](const auto& entry, auto value) { return entry.first < value; });
    }
    std::optional<RowLocation> integer_lookup(std::int64_t key) const {
        const auto& bucket = integers[static_cast<std::uint64_t>(key) % integers.size()];
        if (!bucket)
            return {};
        auto found = lower(*bucket, key);
        return found != bucket->end() && found->first == key ? std::optional(found->second) : std::nullopt;
    }
    std::shared_ptr<Index> clone() const override { return std::make_shared<HashIndex>(*this); }
    void insert(const Value& value, RowLocation location) override {
        if (native) {
            const auto key = i64_payload(value);
            auto& bucket = writable_int(number(value));
            auto at = lower(bucket, key);
            if (at != bucket.end() && at->first == key)
                throw Error(ErrorCode::constraint, "Duplicate primary key");
            bucket.insert(at, {key, location});
            return;
        }
        if (!writable(number(value)).emplace(value, location).second)
            throw Error(ErrorCode::constraint, "Duplicate primary key");
    }
    void erase(const Value& value, RowLocation location) override {
        if (lookup(value) != location)
            throw Error(ErrorCode::state, "Missing or mismatched primary index entry");
        if (native) {
            auto& bucket = writable_int(number(value));
            bucket.erase(lower(bucket, i64_payload(value)));
        } else
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
        if (native)
            return integer_lookup(i64_payload(value));
        const auto& bucket = buckets[number(value)];
        if (!bucket)
            return {};
        const auto found = bucket->find(value);
        return found == bucket->end() ? std::nullopt : std::optional(found->second);
    }
    std::optional<IndexResult> range(const Value* lo, const Value* hi) const override {
        IndexResult result;
        if (native) {
            const auto first = lo ? i64_payload(*lo) : INT64_MIN;
            const auto last = hi ? i64_payload(*hi) : INT64_MAX;
            if (first > last)
                return result;
            // A span narrower than the partition count touches each bucket at
            // most once. Probe only those residues, without signed overflow.
            if (static_cast<std::uint64_t>(last) - static_cast<std::uint64_t>(first) < integers.size()) {
                for (auto key = first;; ++key) {
                    if (auto location = integer_lookup(key))
                        result.rows.push_back(*location);
                    if (key == last)
                        break;
                }
            } else {
                for (const auto& bucket : integers)
                    if (bucket)
                        for (auto at = lower(*bucket, first); at != bucket->end() && at->first <= last; ++at)
                            result.rows.push_back(at->second);
            }
        } else {
            if (!less.addon->compare)
                return {};
            for (const auto& bucket : buckets)
                if (bucket)
                    for (auto at = lo ? bucket->lower_bound(*lo) : bucket->begin();
                         at != bucket->end() && (!hi || !less(*hi, at->first)); ++at)
                        result.rows.push_back(at->second);
        }
        return result;
    }
};
} // namespace
std::shared_ptr<Index> make_hash_index(const Type& type, const TypeAddon& addon) {
    return std::make_shared<HashIndex>(type, addon);
}
std::optional<RowLocation> Index::lookup(const Value&) const {
    throw Error(ErrorCode::unsupported, "Index has no unique lookup");
}
std::optional<IndexResult> Index::range(const Value*, const Value*) const {
    return {};
}
void Index::validate_search(const std::string&) const {
    throw Error(ErrorCode::unsupported, "Unsupported index search");
}
IndexResult Index::search(const std::string&, const Value&) const {
    throw Error(ErrorCode::unsupported, "Unsupported index search");
}
bool Index::matches(const std::string&, const Value&, const Value&) const {
    throw Error(ErrorCode::unsupported, "Unsupported index predicate");
}
} // namespace coresql

namespace coresql {
void Index::validate(std::span<const IndexEntry>) const {
    throw Error(ErrorCode::unsupported, "Index does not support integrity validation");
}
Type Index::search_type(const std::string& operation, const Type& source) const {
    validate_search(operation);
    return source;
}
} // namespace coresql
