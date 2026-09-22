#include "storage.hpp"
#include "index.hpp"
#include "paged_index.hpp"
#include "coresql/encoding.hpp"
#include <array>
#include <algorithm>
#include <unordered_map>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

namespace coresql::detail {
namespace {
constexpr std::string_view magic = "CORELOG2";
constexpr std::string_view committed = "COREEND1";
constexpr std::uint64_t max_record = max_encoded_bytes;
#ifdef CORESQL_TESTING
thread_local StorageHook hook = nullptr;
void point(const char* name) {
    if (hook)
        hook(name);
}
#else
void point(const char*) {
}
#endif
[[noreturn]] void io(const char* operation) {
    throw Error(ErrorCode::io, std::string(operation) + ": " + std::strerror(errno));
}
void synchronize(int fd) {
    int rc;
#ifdef __APPLE__
    do {
        rc = ::fcntl(fd, F_FULLFSYNC);
    } while (rc < 0 && errno == EINTR);
#else
    do {
        rc = ::fsync(fd);
    } while (rc < 0 && errno == EINTR);
#endif
    if (rc < 0)
        io("Synchronize database");
}
void write_all(int fd, ByteView data, std::atomic<std::uint64_t>& written) {
    while (!data.empty()) {
        auto n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            io("Write database");
        written += static_cast<std::uint64_t>(n);
        data = data.subspan(static_cast<std::size_t>(n));
    }
}
void read_at(int fd, std::span<std::byte> bytes, off_t offset) {
    while (!bytes.empty()) {
        auto n = ::pread(fd, bytes.data(), bytes.size(), offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            io("Read database");
        if (n == 0)
            throw Error(ErrorCode::format, "Unexpected database end");
        bytes = bytes.subspan(static_cast<std::size_t>(n));
        offset += n;
    }
}
void sync_directory(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        io("Open parent directory");
    int rc;
    do {
        rc = ::fsync(fd);
    } while (rc < 0 && errno == EINTR);
    int error = errno;
    ::close(fd);
    errno = error;
    if (rc < 0)
        io("Synchronize parent directory");
}
} // namespace
#ifdef CORESQL_TESTING
void storage_hook(StorageHook value) {
    hook = value;
}
#endif
DurableStore::DurableStore(const std::filesystem::path& path, bool create_only)
    : path_(std::filesystem::absolute(path)) {
    bool created = false;
    if (create_only) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        created = fd_ >= 0;
    } else
        fd_ = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (!create_only && fd_ < 0 && errno == ENOENT) {
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        created = fd_ >= 0;
        if (fd_ < 0 && errno == EEXIST)
            fd_ = ::open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd_ < 0)
        io("Open database");
    try {
        struct stat status{};
        if (::fstat(fd_, &status) < 0)
            io("Stat database");
        if (!S_ISREG(status.st_mode))
            throw Error(ErrorCode::io, "Database must be a regular local file");
        point("open_before_lock");
        if (::flock(fd_, LOCK_EX | LOCK_NB) < 0)
            throw Error(ErrorCode::conflict, "Database is already open; only one owner is supported");
        // A checkpoint can replace the pathname while an opener holds the old
        // inode. Never admit that stale inode after obtaining its lock.
        struct stat named{};
        if (::lstat(path.c_str(), &named) < 0)
            io("Stat database path");
        if (named.st_dev != status.st_dev || named.st_ino != status.st_ino)
            throw Error(ErrorCode::conflict, "Database was replaced while opening; retry");
        if (created) {
            write_all(fd_, std::as_bytes(std::span(magic.data(), magic.size())), bytes_written);
            synchronize(fd_);
        }
        // Also covers reopening a file after interruption during its creation.
        sync_directory(path);
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}
struct CheckpointImages {
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> span;
};
DurableStore::~DurableStore() {
    if (image_fd_ >= 0)
        ::close(image_fd_);
    if (fd_ >= 0)
        ::close(fd_);
}
State DurableStore::recover(const Registry& registry, const std::shared_ptr<Pager>& pager) {
    struct stat status{};
    if (::fstat(fd_, &status) < 0)
        io("Stat database");
    if (status.st_size < 8)
        throw Error(ErrorCode::format, "Truncated database header");
    std::array<std::byte, 8> header{};
    read_at(fd_, header, 0);
    if (std::memcmp(header.data(), magic.data(), 8) != 0)
        throw Error(ErrorCode::format, "Unsupported database format");
    auto fingerprint = [&](ByteView bytes) {
        if (!pager)
            return;
        for (auto byte : bytes) {
            recovery_fingerprint_ ^= std::to_integer<unsigned>(byte);
            recovery_fingerprint_ *= 1099511628211ULL;
        }
    };
    recovery_fingerprint_ = 14695981039346656037ULL;
    fingerprint(header);
    off_t position = 8;
    State latest;
    while (position < status.st_size) {
        if (status.st_size - position < 16)
            break; // incomplete final record header
        std::array<std::byte, 16> frame{};
        read_at(fd_, frame, position);
        encoding::Reader reader(frame);
        auto size = reader.u64(), inverse = reader.u64();
        if (inverse != ~size || size < 32 || size > max_record)
            throw Error(ErrorCode::format, "Corrupt transaction frame header");
        if (size + 8 > static_cast<std::uint64_t>(status.st_size - position - 16))
            break;
        std::array<std::byte, 8> marker{};
        read_at(fd_, marker, position + 16 + static_cast<off_t>(size));
        if (std::memcmp(marker.data(), committed.data(), 8) != 0)
            throw Error(ErrorCode::format, "Corrupt transaction commit marker");
        // Mapping the record avoids a second anonymous allocation the size of a
        // checkpoint. Its bytes remain immutable while this owner recovers.
        const auto page = static_cast<off_t>(::sysconf(_SC_PAGESIZE));
        if (page <= 0)
            io("Read memory page size");
        const off_t payload_offset = position + 16;
        const off_t mapped_offset = payload_offset / page * page;
        const auto prefix = static_cast<std::size_t>(payload_offset - mapped_offset);
        const auto mapped_size = prefix + static_cast<std::size_t>(size);
        void* mapping = ::mmap(nullptr, mapped_size, PROT_READ, MAP_PRIVATE, fd_, mapped_offset);
        if (mapping == MAP_FAILED)
            io("Map recovery record");
        try {
            ByteView data(static_cast<const std::byte*>(mapping) + prefix, static_cast<std::size_t>(size));
            latest = apply_changes(latest, data, registry, pager);
            fingerprint(frame);
            fingerprint(data);
            fingerprint(marker);
        } catch (...) {
            ::munmap(mapping, mapped_size);
            throw;
        }
        ::munmap(mapping, mapped_size);
        position += 24 + static_cast<off_t>(size);
    }
    if (position != status.st_size) {
        if (::ftruncate(fd_, position) < 0)
            io("Remove interrupted transaction tail");
    }
    // A complete record from an interrupted commit is adopted and made durable.
    synchronize(fd_);
    if (::lseek(fd_, position, SEEK_SET) < 0)
        io("Seek database");
    committed_end_ = position;
    // This reserved sibling can only be an unpublished checkpoint left by a
    // previous owner. The current inode lock and pathname check exclude writers.
    auto temporary = path_.string() + ".checkpoint";
    if (::unlink(temporary.c_str()) < 0 && errno != ENOENT)
        io("Remove abandoned checkpoint");
    temporary = path_.string() + ".checkpoint.background";
    if (::unlink(temporary.c_str()) < 0 && errno != ENOENT)
        io("Remove abandoned background checkpoint");
    return latest;
}
void DurableStore::append(int fd, ByteView payload) {
    Bytes header;
    encoding::u64(header, payload.size());
    encoding::u64(header, ~std::uint64_t(payload.size()));
    point("before_write");
    write_all(fd, ByteView(header).first(8), bytes_written);
    point("half_header");
    write_all(fd, ByteView(header).subspan(8), bytes_written);
    point("header");
    auto half = payload.size() / 2;
    write_all(fd, payload.first(half), bytes_written);
    point("half_payload");
    write_all(fd, payload.subspan(half), bytes_written);
    point("payload");
    synchronize(fd);
    point("payload_synced");
    auto marker = std::as_bytes(std::span(committed.data(), committed.size()));
    write_all(fd, marker.first(4), bytes_written);
    point("half_footer");
    write_all(fd, marker.subspan(4), bytes_written);
    point("footer");
    point("before_sync");
    synchronize(fd);
    point("synced");
}
void DurableStore::commit(const State& base, const State& next) {
    std::lock_guard lock(io_mutex_);
    if (failed)
        throw Error(ErrorCode::state, "Database must be reopened after I/O failure");
    if (base.schema_epoch != next.schema_epoch) {
        checkpoint_locked(next);
        return;
    }
    auto payload = encode_changes(base, next); // Allocation/encoding fails before I/O.
    struct stat status{};
    if (::fstat(fd_, &status) < 0)
        io("Stat database");
    const auto live = checkpoint_size(next) + 32;
    // Bound retained history relative to live data, with a 1 MiB floor to avoid
    // rewriting tiny databases too frequently. Shrinking data triggers cleanup.
    const auto threshold = std::max<std::uint64_t>(1024 * 1024, 4 * live);
    if (static_cast<std::uint64_t>(status.st_size) + payload.size() + 24 > threshold) {
        checkpoint_locked(next);
        return;
    }
    try {
        append(fd_, payload);
        committed_end_ += static_cast<std::int64_t>(payload.size()) + 24;
    } catch (...) {
        failed = true;
        throw;
    }
}
void DurableStore::checkpoint(const State& state) {
    std::lock_guard lock(io_mutex_);
    checkpoint_locked(state);
}
void DurableStore::checkpoint_locked(const State& state) {
    if (failed)
        throw Error(ErrorCode::state, "Database must be reopened after I/O failure");
    checkpoint_size(state);
    auto temporary = path_.string() + ".checkpoint";
    int fresh = ::open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fresh < 0)
        io("Create checkpoint");
    try {
        if (::flock(fresh, LOCK_EX | LOCK_NB) < 0)
            io("Lock checkpoint");
        point("checkpoint_created");
        write_all(fresh, std::as_bytes(std::span(magic.data(), magic.size())), bytes_written);
        auto built = std::make_unique<CheckpointImages>();
        append_checkpoint(fresh, state, images_.get(), image_fd_, built.get());
        point("checkpoint_synced");
        if (::rename(temporary.c_str(), path_.c_str()) < 0)
            io("Publish checkpoint");
        // Both inodes stay locked across rename. Openers verify pathname identity.
        const auto end = ::lseek(fresh, 0, SEEK_CUR);
        if (end < 0)
            io("Measure checkpoint");
        int old = fd_;
        fd_ = fresh;
        fresh = -1;
        committed_end_ = end;
        ++generation_;
        ::close(old);
        retain_images(std::move(built));
        point("checkpoint_renamed");
        sync_directory(path_);
        ++checkpoints;
        point("checkpoint_directory_synced");
    } catch (...) {
        failed = true;
        if (fresh >= 0) {
            ::close(fresh);
            ::unlink(temporary.c_str());
        }
        throw;
    }
}

// A frame's exact size is known only after streaming the body. Until the complete
// body is written the temporary file is unpublished, so patch its frame header
// before the first synchronization and retain the usual two-sync commit protocol.
void DurableStore::retain_images(std::unique_ptr<CheckpointImages> built) {
    if (image_fd_ >= 0)
        ::close(image_fd_);
    image_fd_ = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
    if (image_fd_ < 0) {
        image_fd_ = -1;
        images_.reset();
        return;
    }
    images_ = std::move(built);
}
void DurableStore::append_checkpoint(int fd, const State& state, const CheckpointImages* reuse, int reuse_fd,
                                     CheckpointImages* built) {
    const auto header_offset = ::lseek(fd, 0, SEEK_CUR);
    if (header_offset < 0)
        io("Seek checkpoint");
    std::array<std::byte, 16> placeholder{};
    point("before_write");
    write_all(fd, ByteView(placeholder).first(8), bytes_written);
    point("half_header");
    write_all(fd, ByteView(placeholder).subspan(8), bytes_written);
    point("header");
    std::uint64_t size = 0;
    bool first = true;
    const auto origin = static_cast<std::uint64_t>(header_offset) + 16;
    encode_checkpoint(
        state,
        [&](ByteView bytes) {
            write_all(fd, bytes, bytes_written);
            size += bytes.size();
            if (first) {
                first = false;
                point("half_payload");
            }
        },
        [&](const Chunk& chunk, const std::function<void(ByteView)>& emit) {
            if (!reuse || reuse_fd < 0)
                return false;
            auto found = reuse->span.find(chunk.encoding_id);
            if (found == reuse->span.end() || found->second.second == 0)
                return false;
            std::array<std::byte, 64 * 1024> buffer{};
            auto remaining = found->second.second;
            auto offset = static_cast<off_t>(found->second.first);
            while (remaining) {
                const auto count = std::min<std::uint64_t>(buffer.size(), remaining);
                read_at(reuse_fd, std::span(buffer).first(static_cast<std::size_t>(count)), offset);
                emit(std::as_bytes(std::span(buffer).first(static_cast<std::size_t>(count))));
                offset += static_cast<off_t>(count);
                remaining -= count;
            }
            return true;
        },
        [&](const Chunk& chunk, std::uint64_t offset, std::uint64_t length) {
            if (built && length)
                built->span[chunk.encoding_id] = {origin + offset, length};
        });
    point("payload");
    Bytes header;
    encoding::u64(header, size);
    encoding::u64(header, ~size);
    auto remaining = ByteView(header);
    auto offset = header_offset;
    while (!remaining.empty()) {
        auto n = ::pwrite(fd, remaining.data(), remaining.size(), offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            io("Complete checkpoint header");
        offset += n;
        bytes_written += static_cast<std::uint64_t>(n);
        remaining = remaining.subspan(static_cast<std::size_t>(n));
    }
    synchronize(fd);
    point("payload_synced");
    auto marker = std::as_bytes(std::span(committed.data(), committed.size()));
    write_all(fd, marker.first(4), bytes_written);
    point("half_footer");
    write_all(fd, marker.subspan(4), bytes_written);
    point("footer");
    point("before_sync");
    synchronize(fd);
    point("synced");
}

DurableStore::Checkpoint::Checkpoint(DurableStore& owner) : owner_(&owner) {
}
DurableStore::Checkpoint::~Checkpoint() {
    if (fresh_ >= 0) {
        ::close(fresh_);
        ::unlink(temporary_.c_str());
    }
    if (source_ >= 0)
        ::close(source_);
    if (owner_) {
        std::lock_guard lock(owner_->io_mutex_);
        owner_->background_active_ = false;
    }
}
std::unique_ptr<DurableStore::Checkpoint> DurableStore::prepare_checkpoint() {
    auto task = std::unique_ptr<Checkpoint>(new Checkpoint(*this));
    std::lock_guard lock(io_mutex_);
    // Only a successful reservation belongs to the token's destructor.
    task->owner_ = nullptr;
    if (failed)
        throw Error(ErrorCode::state, "Database must be reopened after I/O failure");
    if (background_active_)
        throw Error(ErrorCode::conflict, "A background checkpoint is already active");
    task->temporary_ = path_.string() + ".checkpoint.background";
    task->source_ = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
    if (task->source_ < 0)
        io("Retain checkpoint source");
    task->fresh_ = ::open(task->temporary_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (task->fresh_ < 0)
        io("Create background checkpoint");
    if (::flock(task->fresh_, LOCK_EX | LOCK_NB) < 0)
        io("Lock background checkpoint");
    task->generation_ = generation_;
    task->copied_ = committed_end_;
    if (images_)
        task->reuse_ = std::make_unique<CheckpointImages>(*images_);
    task->owner_ = this;
    background_active_ = true;
    return task;
}
void DurableStore::copy_tail(Checkpoint& task, std::int64_t end) {
    std::array<std::byte, 64 * 1024> buffer{};
    while (task.copied_ < end) {
        const auto count =
            std::min<std::uint64_t>(buffer.size(), static_cast<std::uint64_t>(end - task.copied_));
        auto bytes = std::span(buffer).first(static_cast<std::size_t>(count));
        read_at(task.source_, bytes, static_cast<off_t>(task.copied_));
        write_all(task.fresh_, bytes, bytes_written);
        task.copied_ += static_cast<std::int64_t>(count);
    }
}
void DurableStore::write_checkpoint(Checkpoint& task, const State& state) {
    if (task.owner_ != this)
        throw Error(ErrorCode::state, "Invalid background checkpoint token");
    if (!task.ready_) {
        point("background_checkpoint_created");
        write_all(task.fresh_, std::as_bytes(std::span(magic.data(), magic.size())), bytes_written);
        task.built_ = std::make_unique<CheckpointImages>();
        append_checkpoint(task.fresh_, state, task.reuse_.get(), task.source_, task.built_.get());
        point("background_checkpoint_encoded");
    }
    std::int64_t end;
    {
        std::lock_guard lock(io_mutex_);
        if (failed)
            throw Error(ErrorCode::state, "Database must be reopened after I/O failure");
        if (task.generation_ != generation_)
            return;
        end = committed_end_;
    }
    // Copy only acknowledged records. Their bytes never change on this inode,
    // even if a synchronous checkpoint replaces the pathname in the meantime.
    copy_tail(task, end);
    synchronize(task.fresh_);
    task.ready_ = true;
    point("background_checkpoint_ready");
}
bool DurableStore::ready_to_publish(const Checkpoint& task) {
    std::lock_guard lock(io_mutex_);
    if (task.owner_ != this)
        throw Error(ErrorCode::state, "Invalid background checkpoint token");
    return task.generation_ != generation_ || (task.ready_ && committed_end_ - task.copied_ <= 256 * 1024);
}
bool DurableStore::publish_checkpoint(Checkpoint& task) {
    std::lock_guard lock(io_mutex_);
    if (task.owner_ != this)
        throw Error(ErrorCode::state, "Invalid background checkpoint token");
    if (failed)
        throw Error(ErrorCode::state, "Database must be reopened after I/O failure");
    if (task.generation_ != generation_)
        return false;
    if (!task.ready_)
        throw Error(ErrorCode::state, "Background checkpoint has not been encoded");
    if (committed_end_ - task.copied_ > 256 * 1024)
        throw Error(ErrorCode::state, "Background checkpoint requires another catch-up pass");
    // Allocation-free final catch-up while commits are excluded. Earlier copies
    // and most I/O happened outside this short publication boundary.
    copy_tail(task, committed_end_);
    synchronize(task.fresh_);
    point("background_checkpoint_synced");
    try {
        if (::rename(task.temporary_.c_str(), path_.c_str()) < 0)
            io("Publish background checkpoint");
        const auto end = ::lseek(task.fresh_, 0, SEEK_CUR);
        if (end < 0)
            io("Measure background checkpoint");
        int old = fd_;
        fd_ = task.fresh_;
        task.fresh_ = -1;
        committed_end_ = end;
        ++generation_;
        ::close(old);
        retain_images(std::move(task.built_));
        point("background_checkpoint_renamed");
        sync_directory(path_);
        ++checkpoints;
        point("background_checkpoint_directory_synced");
    } catch (...) {
        failed = true;
        throw;
    }
    return true;
}
} // namespace coresql::detail

namespace coresql {
Database Database::open(const std::filesystem::path& path, Registry registry, OpenOptions options) {
    Database database(std::move(registry), options);
    auto storage = std::make_shared<detail::DurableStore>(path);
    auto recovered =
        std::make_shared<detail::State>(storage->recover(database.owner_->registry, database.owner_->pager));
    if (database.owner_->pager)
        detail::rebuild_paged_indexes(*recovered, database.owner_->registry, storage->path(),
                                      storage->recovery_fingerprint());
    else
        detail::rebuild_indexes(*recovered, database.owner_->registry);
    detail::validate_constraints(recovered->tables, database.owner_->registry);
    database.owner_->current = std::move(recovered);
    database.owner_->storage = std::move(storage);
    return database;
}
} // namespace coresql

namespace coresql {
std::size_t Database::encoded_size_limit() {
    return detail::max_encoded_bytes;
}
void Database::backup(const std::filesystem::path& path) const {
    detail::healthy(owner_);
    // Validate the live size before creating the destination.
    const auto snapshot = owner_->capture();
    (void)detail::checkpoint_size(*snapshot);
    auto target = std::make_shared<detail::DurableStore>(path, true);
    target->checkpoint(*snapshot);
}
Database Database::restore(const std::filesystem::path& snapshot, const std::filesystem::path& destination,
                           Registry registry) {
    auto database = load(snapshot, std::move(registry));
    database.backup(destination);
    return open(destination, database.owner_->registry);
}
} // namespace coresql
