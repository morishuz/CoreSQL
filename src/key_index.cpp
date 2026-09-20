#include "key_index.hpp"
#include "comparison.hpp"
#include <array>
#include <bit>
#include <unordered_map>

namespace coresql::detail {
std::vector<Value> KeyBinding::keys(const Value& value) const {
    auto result = extractor.extract(value);
    for (const auto& key : result) {
        if (is_null(key))
            throw Error(ErrorCode::type, "Extractor returned a NULL key");
        if (!holds_layout(addon, extractor.key_type, key))
            throw Error(ErrorCode::type, "Extractor returned the wrong key type");
        addon.validate_value(extractor.key_type.parameters, key);
    }
    return result;
}
bool KeyBinding::matches(const Value& source, const Value& key) const {
    for (const auto& candidate : keys(source))
        if (addon.equal(extractor.key_type.parameters, candidate, key)) return true;
    return false;
}

namespace {
struct KeyIndex final : Index {
    struct Hash {
        std::shared_ptr<const KeyBinding> binding;
        std::size_t operator()(const Value& key) const {
            return binding->addon.hash(binding->extractor.key_type.parameters, key);
        }
    };
    struct Equal {
        std::shared_ptr<const KeyBinding> binding;
        bool operator()(const Value& a, const Value& b) const {
            return binding->addon.equal(binding->extractor.key_type.parameters, a, b);
        }
    };
    // Sparse 128-slot masks avoid an allocation per matching row. Duplicate keys
    // in a row set the same bit; there is no per-document occurrence count.
    using Mask = std::array<std::uint64_t, 2>;
    using Posting = std::map<RowLocation, Mask>;
    using Bucket = std::unordered_map<Value, std::shared_ptr<Posting>, Hash, Equal>;
    std::shared_ptr<const KeyBinding> binding;
    std::array<std::shared_ptr<Bucket>, 256> buckets{};
    KeyIndex(const KeyExtractor& extractor, const TypeAddon& addon)
        : binding(std::make_shared<KeyBinding>(KeyBinding{extractor, addon})) {}
    std::shared_ptr<Index> clone() const override { return std::make_shared<KeyIndex>(*this); }
    void change(const Value& source, RowLocation location, bool inserting) {
        auto keys = binding->keys(source);
        const RowLocation block{location.chunk, location.slot / 128 * 128};
        const auto word = (location.slot % 128) / 64;
        const auto bit = std::uint64_t{1} << (location.slot % 64);
        // Stage every affected bucket so any extractor/hash/allocation failure is atomic.
        // Unaffected buckets and postings remain shared between snapshots.
        std::map<std::size_t, std::shared_ptr<Bucket>> changed;
        for (const auto& key : keys) {
            const auto n = Hash{binding}(key) % buckets.size();
            auto [it, fresh] = changed.try_emplace(n);
            if (fresh) it->second = buckets[n] ? std::make_shared<Bucket>(*buckets[n])
                : std::make_shared<Bucket>(0, Hash{binding}, Equal{binding});
            auto& bucket = *it->second;
            if (inserting) {
                auto& posting = bucket[key];
                if (!posting) posting = std::make_shared<Posting>();
                else if (posting.use_count() != 1) posting = std::make_shared<Posting>(*posting);
                (*posting)[block][word] |= bit;
            } else {
                auto entry = bucket.find(key);
                if (entry == bucket.end()) continue; // duplicate emission already removed
                auto& posting = entry->second;
                if (posting.use_count() != 1) posting = std::make_shared<Posting>(*posting);
                auto found = posting->find(block);
                if (found == posting->end()) continue;
                found->second[word] &= ~bit;
                if (!(found->second[0] | found->second[1])) posting->erase(found);
                if (posting->empty()) bucket.erase(entry);
            }
        }
        for (auto& [n, bucket] : changed) buckets[n] = std::move(bucket);
    }
    void insert(const Value& value, RowLocation location) override { change(value, location, true); }
    void erase(const Value& value, RowLocation location) override { change(value, location, false); }
    void validate(std::span<const IndexEntry> source) const override {
        KeyIndex expected(binding->extractor, binding->addon);
        for (const auto& entry : source)
            expected.insert(entry.value, entry.location);
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            const auto& actual = buckets[i];
            const auto& wanted = expected.buckets[i];
            if ((actual ? actual->size() : 0) != (wanted ? wanted->size() : 0))
                throw Error(ErrorCode::state, "Extracted-key index cardinality mismatch");
            if (wanted)
                for (const auto& [key, posting] : *wanted) {
                    auto found = actual->find(key);
                    if (found == actual->end() || !found->second || *found->second != *posting)
                        throw Error(ErrorCode::state, "Extracted-key index mismatch");
                }
        }
    }
    void validate_search(const std::string& operation) const override {
        if (operation != "equal") throw Error(ErrorCode::unsupported, "Key index supports equality membership only");
    }
    Type search_type(const std::string& operation, const Type&) const override {
        validate_search(operation); return binding->extractor.key_type;
    }
    IndexResult search(const std::string& operation, const Value& key) const override {
        validate_search(operation);
        const auto& bucket = buckets[Hash{binding}(key) % buckets.size()];
        if (!bucket) return {};
        auto found = bucket->find(key);
        if (found == bucket->end()) return {};
        std::vector<RowLocation> result;
        std::size_t count = 0;
        for (const auto& [block, mask] : *found->second) { (void)block; count += static_cast<std::size_t>(std::popcount(mask[0]) + std::popcount(mask[1])); }
        result.reserve(count);
        for (const auto& [block, mask] : *found->second) {
            for (std::uint32_t word = 0; word < 2; ++word) {
                auto bits = mask[word];
                while (bits) {
                    result.push_back({block.chunk, block.slot + word * 64 + static_cast<std::uint32_t>(std::countr_zero(bits))});
                    bits &= bits - 1;
                }
            }
        }
        return {std::move(result), true};
    }
    bool matches(const std::string& operation, const Value& source, const Value& key) const override {
        validate_search(operation);
        return binding->matches(source, key);
    }
};
}
std::shared_ptr<Index> make_key_index(const KeyExtractor& extractor, const TypeAddon& addon) {
    return std::make_shared<KeyIndex>(extractor, addon);
}
}
