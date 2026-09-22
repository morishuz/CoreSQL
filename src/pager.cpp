#include "pager.hpp"
#include "storage.hpp"
#include "paged_index.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace coresql::detail {
namespace {
[[noreturn]] void io(const char* what) {
    throw Error(ErrorCode::io, std::string(what) + ": " + std::strerror(errno));
}
std::size_t footprint(const Chunk& chunk) {
    auto size = sizeof(Chunk) + chunk.rows.capacity() * sizeof(Row) +
                chunk.rowids.capacity() * sizeof(std::int64_t) +
                chunk.slots.capacity() * sizeof(std::uint32_t);
    for (const auto& row : chunk.rows) {
        size += row.capacity() * sizeof(Value);
        for (const auto& value : row) {
            if (const auto* s = std::get_if<std::string>(&value))
                size += s->capacity();
            else if (const auto* bytes = std::get_if<Opaque>(&value))
                size += bytes->bytes().size();
        }
    }
    return size;
}
} // namespace
Page::~Page() {
    if (owner && length)
        owner->release(offset, length);
}
std::shared_ptr<Chunk> ChunkRef::pin() const {
    if (resident_)
        return resident_;
    if (page_)
        return page_->owner->pin(page_);
    throw Error(ErrorCode::state, "Missing chunk");
}
std::shared_ptr<Chunk>& ChunkRef::writable() {
    if (page_) {
        resident_ = std::make_shared<Chunk>(*pin());
        page_.reset();
    } else if (resident_ && resident_.use_count() != 1)
        resident_ = std::make_shared<Chunk>(*resident_);
    return resident_;
}
std::size_t ChunkRef::rows() const {
    return page_ ? page_->row_count : resident_->rows.size();
}
std::size_t ChunkRef::payload_bytes() const {
    return page_ ? page_->payload_bytes : resident_->payload_bytes;
}
std::size_t ChunkRef::encoded_bytes() const {
    return page_ ? page_->encoded_bytes : resident_->encoded_bytes;
}
Pager::Pager(std::size_t target, Registry registry) : registry_(std::move(registry)), target_(target) {
    auto pattern = (std::filesystem::temp_directory_path() / "coresql-pages-XXXXXX").string();
    fd_ = ::mkstemp(pattern.data());
    if (fd_ < 0)
        io("Create page backing");
    if (::unlink(pattern.c_str()) < 0 || ::fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) {
        const auto error = errno;
        ::close(fd_);
        fd_ = -1;
        errno = error;
        io("Prepare page backing");
    }
}
Pager::~Pager() {
    if (fd_ >= 0)
        ::close(fd_);
}
void Pager::evict(std::size_t target) {
    for (auto it = cache_.begin(); it != cache_.end() && resident_ > target;) {
        // A live reader pin always wins over the cache target. Never invalidate
        // borrowed Values to enforce a memory target.
        if ((resident_ > target || it->second.page.expired()) && it->second.chunk.use_count() == 1) {
            resident_ -= it->second.bytes;
            lookup_.erase(it->first);
            it = cache_.erase(it);
            ++evictions_;
        } else
            ++it;
    }
}
void Pager::retain(const std::shared_ptr<Page>& page, std::shared_ptr<Chunk> chunk) {
    auto found = lookup_.find(page->id);
    if (found != lookup_.end()) {
        cache_.splice(cache_.end(), cache_, found->second);
        return;
    }
    const auto bytes = footprint(*chunk);
    cache_.emplace_back(page->id, Entry{page, std::move(chunk), bytes});
    try {
        lookup_.emplace(page->id, std::prev(cache_.end()));
    } catch (...) {
        cache_.pop_back();
        throw;
    }
    resident_ += bytes;
}
ChunkRef Pager::store(std::shared_ptr<Chunk> chunk, std::shared_ptr<const std::vector<Column>> columns) {
    auto bytes = encode_page(*chunk);
    auto page = std::make_shared<Page>();
    page->owner = shared_from_this();
    page->columns = std::move(columns);
    // length is published only once the extent has been reserved.
    page->row_count = chunk->rows.size();
    page->payload_bytes = chunk->payload_bytes;
    page->encoded_bytes = chunk->encoded_bytes;
    std::lock_guard lock(mutex_);
    if (bytes.size() > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - end_)
        throw Error(ErrorCode::state, "Page backing exhausted file offset range");
    page->offset = end_;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->second < bytes.size())
            continue;
        page->offset = it->first;
        const auto excess = it->second - bytes.size();
        // Insert the remainder before erasing to preserve extents on allocation failure.
        if (excess)
            free_.emplace(it->first + bytes.size(), excess);
        free_.erase(it);
        break;
    }
    page->length = bytes.size();
    end_ = std::max(end_, page->offset + page->length);
    page->id = next_++;
    auto offset = page->offset;
    ByteView remaining(bytes);
    while (!remaining.empty()) {
        auto n = ::pwrite(fd_, remaining.data(), remaining.size(), static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            io("Write page backing");
        offset += static_cast<std::uint64_t>(n);
        remaining = remaining.subspan(static_cast<std::size_t>(n));
    }
    ++writes_;
    page->loaded = chunk;
    retain(page, std::move(chunk));
    evict(target_);
    return ChunkRef(std::move(page));
}
std::shared_ptr<Chunk> Pager::pin(const std::shared_ptr<Page>& page) {
    std::lock_guard lock(mutex_);
    auto chunk = page->loaded.lock();
    if (!chunk) {
        Bytes data(page->length);
        std::span<std::byte> remaining(data);
        auto offset = page->offset;
        while (!remaining.empty()) {
            auto n = ::pread(fd_, remaining.data(), remaining.size(), static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                io("Read page backing");
            if (!n)
                throw Error(ErrorCode::format, "Truncated page backing");
            offset += static_cast<std::uint64_t>(n);
            remaining = remaining.subspan(static_cast<std::size_t>(n));
        }
        chunk = decode_page(data, *page->columns, registry_);
        page->loaded = chunk;
        ++reads_;
    }
    retain(page, chunk);
    evict(target_);
    return chunk;
}
CacheStats Pager::stats() const {
    std::lock_guard lock(mutex_);
    CacheStats result;
    result.target_bytes = target_;
    result.resident_bytes = resident_;
    result.backing_bytes = end_;
    for (const auto& [id, entry] : cache_) {
        (void)id;
        if (entry.chunk.use_count() > 1)
            result.pinned_bytes += entry.bytes;
    }
    result.overage_bytes = resident_ > target_ ? resident_ - target_ : 0;
    result.page_reads = reads_;
    result.page_writes = writes_;
    result.evictions = evictions_;
    return result;
}
void Pager::trim() {
    std::lock_guard lock(mutex_);
    evict(0);
}
void Pager::trim_to_target() {
    std::lock_guard lock(mutex_);
    evict(target_);
}
void Pager::release(std::uint64_t offset, std::size_t length) noexcept {
    try {
        std::lock_guard lock(mutex_);
        auto [current, inserted] = free_.emplace(offset, length);
        if (!inserted)
            return;
        if (current != free_.begin()) {
            auto before = std::prev(current);
            if (before->first + before->second == current->first) {
                before->second += current->second;
                free_.erase(current);
                current = before;
            }
        }
        auto after = std::next(current);
        if (after != free_.end() && current->first + current->second == after->first) {
            current->second += after->second;
            free_.erase(after);
        }
        if (current->first + current->second == end_ &&
            ::ftruncate(fd_, static_cast<off_t>(current->first)) == 0) {
            end_ = current->first;
            free_.erase(current);
        }
        evict(target_);
    } catch (...) {
        // Destruction cannot throw; an allocation failure may leave one unused
        // extent until this backing file closes, without affecting stored rows.
    }
}
void page_state(State& state, const std::shared_ptr<Pager>& pager) {
    if (!pager)
        return;
    for (auto& [name, stored] : state.tables) {
        (void)name;
        std::shared_ptr<Table> edited;
        std::shared_ptr<const std::vector<Column>> columns;
        for (const auto& [id, chunk] : stored->chunks) {
            if (chunk.paged())
                continue;
            if (!edited) {
                edited = std::make_shared<Table>(*stored);
                columns = std::make_shared<const std::vector<Column>>(stored->columns);
            }
            edited->chunks[id] = pager->store(chunk.pin(), columns);
        }
        if (edited) {
            page_primary_index(*edited, pager->registry());
            stored = std::move(edited);
        }
    }
    pager->trim_to_target();
}
} // namespace coresql::detail
