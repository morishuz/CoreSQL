#include "chunk_map.hpp"

namespace coresql::detail {
std::size_t ChunkMap::encoded_bytes() const {
    std::size_t total = 0;
    for (const auto& block : blocks_) {
        auto bytes = block->encoded.load(std::memory_order_relaxed);
        if (bytes == unknown_size) {
            bytes = 0;
            for (const auto& [id, chunk] : block->entries) {
                (void)id;
                bytes += chunk.encoded_bytes();
            }
            // Concurrent checkpoint readers can compute the same immutable
            // block's size. Mutable access invalidates only its private block.
            block->encoded.store(bytes, std::memory_order_relaxed);
        }
        total += bytes;
    }
    return total;
}

std::vector<ChunkMap::Change> ChunkMap::changes_from(const ChunkMap& previous) const {
    std::vector<Change> changes;
    std::size_t a = 0, b = 0;
    while (a < blocks_.size() || b < previous.blocks_.size()) {
        std::span<const Entry> after, before;
        if (b == previous.blocks_.size() ||
            (a < blocks_.size() && blocks_[a]->number < previous.blocks_[b]->number)) {
            after = blocks_[a++]->entries;
        } else if (a == blocks_.size() || previous.blocks_[b]->number < blocks_[a]->number) {
            before = previous.blocks_[b++]->entries;
        } else {
            const bool shared = blocks_[a] == previous.blocks_[b];
            after = blocks_[a++]->entries;
            before = previous.blocks_[b++]->entries;
            if (shared)
                continue;
        }
        std::size_t i = 0, j = 0;
        while (i < after.size() || j < before.size()) {
            if (j == before.size() || (i < after.size() && after[i].first < before[j].first)) {
                changes.push_back(after[i++]);
            } else if (i == after.size() || before[j].first < after[i].first) {
                changes.emplace_back(before[j++].first, ChunkRef{});
            } else {
                if (after[i].second != before[j].second)
                    changes.push_back(after[i]);
                ++i;
                ++j;
            }
        }
    }
    return changes;
}
} // namespace coresql::detail
