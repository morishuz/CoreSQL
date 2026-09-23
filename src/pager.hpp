#pragma once
#include "state.hpp"
#include <list>
#include <mutex>
#include <unordered_map>

namespace coresql::detail {
class Pager;
struct Page {
    ~Page();
    std::shared_ptr<Pager> owner;
    std::shared_ptr<const std::vector<Column>> columns;
    std::uint64_t id = 0, offset = 0;
    std::size_t length = 0, row_count = 0, payload_bytes = 0, encoded_bytes = 0;
    std::weak_ptr<Chunk> loaded;
};
// Pages are private, unlinked scratch data, never a substitute for durable commit.
// Immutable ChunkRefs retain their backing file; pins retain decoded values.
class Pager : public std::enable_shared_from_this<Pager> {
public:
    Pager(std::size_t target, Registry registry);
    ~Pager();
    ChunkRef store(std::shared_ptr<Chunk>, std::shared_ptr<const std::vector<Column>>);
    std::shared_ptr<Chunk> pin(const std::shared_ptr<Page>&);
    CacheStats stats() const;
    const Registry& registry() const { return registry_; }
    void trim();
    void trim_to_target();
    void release(std::uint64_t offset, std::size_t length) noexcept;

private:
    struct Entry {
        std::weak_ptr<Page> page;
        std::shared_ptr<Chunk> chunk;
        std::size_t bytes;
    };
    mutable std::mutex mutex_;
    int fd_ = -1;
    Registry registry_;
    std::size_t target_ = 0, resident_ = 0;
    std::uint64_t end_ = 0, next_ = 0, reads_ = 0, writes_ = 0, evictions_ = 0;
    std::map<std::uint64_t, std::size_t> free_;
    std::list<std::pair<std::uint64_t, Entry>> cache_;
    std::unordered_map<std::uint64_t, decltype(cache_)::iterator> lookup_;
    void evict(std::size_t target);
    void retain(const std::shared_ptr<Page>&, std::shared_ptr<Chunk>);
};
void page_state(State&, const State& base, const std::shared_ptr<Pager>&);
Bytes encode_page(const Chunk&);
std::shared_ptr<Chunk> decode_page(ByteView, const std::vector<Column>&, const Registry&);
} // namespace coresql::detail
