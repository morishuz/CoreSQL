#include "storage.hpp"
#include "index.hpp"
#include "coresql/encoding.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace coresql::detail {
namespace {
constexpr std::string_view magic = "CORELOG2";
constexpr std::string_view committed = "COREEND1";
constexpr std::uint64_t max_record = max_encoded_bytes;
#ifdef CORESQL_TESTING
thread_local StorageHook hook = nullptr;
void point(const char* name) { if(hook) hook(name); }
#else
void point(const char*) {}
#endif
[[noreturn]] void io(const char* operation) {
    throw Error(ErrorCode::io, std::string(operation) + ": " + std::strerror(errno));
}
void synchronize(int fd) {
    int rc;
#ifdef __APPLE__
    do { rc = ::fcntl(fd, F_FULLFSYNC); } while (rc < 0 && errno == EINTR);
#else
    do { rc = ::fsync(fd); } while (rc < 0 && errno == EINTR);
#endif
    if(rc < 0) io("Synchronize database");
}
void write_all(int fd, ByteView data, std::uint64_t& written) {
    while(!data.empty()) {
        auto n = ::write(fd, data.data(), data.size());
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) io("Write database");
        written += static_cast<std::uint64_t>(n);
        data = data.subspan(static_cast<std::size_t>(n));
    }
}
void read_at(int fd, std::span<std::byte> bytes, off_t offset) {
    while(!bytes.empty()) {
        auto n = ::pread(fd, bytes.data(), bytes.size(), offset);
        if(n < 0 && errno == EINTR) continue;
        if(n < 0) io("Read database");
        if(n == 0) throw Error(ErrorCode::format,"Unexpected database end");
        bytes = bytes.subspan(static_cast<std::size_t>(n)); offset += n;
    }
}
void sync_directory(const std::filesystem::path& path) {
    auto parent = path.parent_path(); if(parent.empty()) parent = ".";
    int fd = ::open(parent.c_str(),O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0) io("Open parent directory");
    int rc; do {rc=::fsync(fd);} while(rc<0 && errno==EINTR);
    int error=errno; ::close(fd); errno=error;
    if(rc<0) io("Synchronize parent directory");
}
}
#ifdef CORESQL_TESTING
void storage_hook(StorageHook value) { hook = value; }
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
        fd_ = ::open(path.c_str(),O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,0600);
        created = fd_ >= 0;
        if(fd_ < 0 && errno == EEXIST) fd_ = ::open(path.c_str(),O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    }
    if(fd_ < 0) io("Open database");
    try {
        struct stat status{}; if(::fstat(fd_,&status)<0) io("Stat database");
        if(!S_ISREG(status.st_mode)) throw Error(ErrorCode::io,"Database must be a regular local file");
        point("open_before_lock");
        if(::flock(fd_,LOCK_EX | LOCK_NB)<0) throw Error(ErrorCode::conflict,"Database is already open; only one owner is supported");
        // A checkpoint can replace the pathname while an opener holds the old
        // inode. Never admit that stale inode after obtaining its lock.
        struct stat named{};
        if (::lstat(path.c_str(), &named) < 0) io("Stat database path");
        if (named.st_dev != status.st_dev || named.st_ino != status.st_ino)
            throw Error(ErrorCode::conflict, "Database was replaced while opening; retry");
        if(created) {
            write_all(fd_,std::as_bytes(std::span(magic.data(),magic.size())), bytes_written);
            synchronize(fd_);
        }
        // Also covers reopening a file after interruption during its creation.
        sync_directory(path);
    } catch(...) {::close(fd_); fd_=-1; throw;}
}
DurableStore::~DurableStore(){if(fd_>=0)::close(fd_);}
State DurableStore::recover(const Registry& registry) {
    struct stat status{}; if(::fstat(fd_,&status)<0) io("Stat database");
    if(status.st_size < 8) throw Error(ErrorCode::format,"Truncated database header");
    std::array<std::byte,8> header{}; read_at(fd_,header,0);
    if(std::memcmp(header.data(),magic.data(),8)!=0) throw Error(ErrorCode::format,"Unsupported database format");
    off_t position=8;
    State latest;
    while(position < status.st_size) {
        if(status.st_size-position < 16) break; // incomplete final record header
        std::array<std::byte,16> frame{};read_at(fd_,frame,position);
        encoding::Reader reader(frame);
        auto size=reader.u64(), inverse=reader.u64();
        if(inverse != ~size || size < 32 || size > max_record)
            throw Error(ErrorCode::format,"Corrupt transaction frame header");
        if(size + 8 > static_cast<std::uint64_t>(status.st_size-position-16)) break;
        std::array<std::byte,8> marker{};
        read_at(fd_,marker,position+16+static_cast<off_t>(size));
        if(std::memcmp(marker.data(),committed.data(),8)!=0)
            throw Error(ErrorCode::format,"Corrupt transaction commit marker");
        Bytes data(static_cast<std::size_t>(size));read_at(fd_,data,position+16);
        validate_snapshot(data); // A complete corrupt record is never silently ignored.
        latest=apply_changes(latest, data, registry);
        position += 24 + static_cast<off_t>(size);
    }
    if(position != status.st_size) {
        if(::ftruncate(fd_,position)<0) io("Remove interrupted transaction tail");
    }
    // A complete record from an interrupted commit is adopted and made durable.
    synchronize(fd_);
    if(::lseek(fd_,position,SEEK_SET)<0) io("Seek database");
    // This reserved sibling can only be an unpublished checkpoint left by a
    // previous owner. The current inode lock and pathname check exclude writers.
    auto temporary = path_.string() + ".checkpoint";
    if (::unlink(temporary.c_str()) < 0 && errno != ENOENT) io("Remove abandoned checkpoint");
    return latest;
}
void DurableStore::append(int fd, ByteView payload) {
    Bytes header; encoding::u64(header,payload.size()); encoding::u64(header,~std::uint64_t(payload.size()));
    point("before_write");
    write_all(fd,ByteView(header).first(8),bytes_written); point("half_header");
    write_all(fd,ByteView(header).subspan(8),bytes_written); point("header");
    auto half=payload.size()/2;
    write_all(fd,payload.first(half),bytes_written); point("half_payload");
    write_all(fd,payload.subspan(half),bytes_written); point("payload");
    synchronize(fd); point("payload_synced");
    auto marker=std::as_bytes(std::span(committed.data(),committed.size()));
    write_all(fd,marker.first(4),bytes_written); point("half_footer");
    write_all(fd,marker.subspan(4),bytes_written); point("footer");
    point("before_sync"); synchronize(fd); point("synced");
}
void DurableStore::commit(const State& base, const State& next) {
    if(failed) throw Error(ErrorCode::state,"Database must be reopened after I/O failure");
    if (base.schema_epoch != next.schema_epoch) {
        checkpoint(next);
        return;
    }
    auto payload = encode_changes(base, next); // Allocation/encoding fails before I/O.
    struct stat status{}; if (::fstat(fd_, &status) < 0) io("Stat database");
    const auto live = checkpoint_size(next) + 32;
    // Bound retained history relative to live data, with a 1 MiB floor to avoid
    // rewriting tiny databases too frequently. Shrinking data triggers cleanup.
    const auto threshold = std::max<std::uint64_t>(1024 * 1024, 4 * live);
    if (static_cast<std::uint64_t>(status.st_size) + payload.size() + 24 > threshold) {
        checkpoint(next);
        return;
    }
    try { append(fd_, payload); }
    catch (...) { failed = true; throw; }
}
void DurableStore::checkpoint(const State& state) {
    if(failed) throw Error(ErrorCode::state,"Database must be reopened after I/O failure");
    auto payload = encode_changes(State{}, state);
    auto temporary = path_.string() + ".checkpoint";
    int fresh = ::open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fresh < 0) io("Create checkpoint");
    try {
        if (::flock(fresh, LOCK_EX | LOCK_NB) < 0) io("Lock checkpoint");
        point("checkpoint_created");
        write_all(fresh, std::as_bytes(std::span(magic.data(), magic.size())), bytes_written);
        append(fresh, payload);
        point("checkpoint_synced");
        if (::rename(temporary.c_str(), path_.c_str()) < 0) io("Publish checkpoint");
        // Both inodes stay locked across rename. Openers verify pathname identity.
        int old = fd_; fd_ = fresh; fresh = -1; ::close(old);
        point("checkpoint_renamed");
        sync_directory(path_);
        ++checkpoints;
        point("checkpoint_directory_synced");
    } catch (...) {
        failed = true;
        if (fresh >= 0) { ::close(fresh); ::unlink(temporary.c_str()); }
        throw;
    }
}
} // namespace coresql::detail

namespace coresql {
Database Database::open(const std::filesystem::path& path, Registry registry) {
    auto storage=std::make_shared<detail::DurableStore>(path);
    auto recovered = std::make_shared<detail::State>(storage->recover(registry));
    detail::rebuild_indexes(*recovered, registry);
    Database database(std::move(registry));
    database.owner_->current = std::move(recovered);
    database.owner_->storage=std::move(storage);
    return database;
}
}

namespace coresql {
std::size_t Database::encoded_size_limit() {
    return detail::max_encoded_bytes;
}
void Database::backup(const std::filesystem::path& path) const {
    detail::healthy(owner_);
    // Validate the live size before creating the destination.
    (void)detail::checkpoint_size(*owner_->current);
    auto target = std::make_shared<detail::DurableStore>(path, true);
    target->commit(detail::State{}, *owner_->current);
}
Database Database::restore(const std::filesystem::path& snapshot, const std::filesystem::path& destination,
                           Registry registry) {
    auto database = load(snapshot, std::move(registry));
    database.backup(destination);
    return open(destination, database.owner_->registry);
}
} // namespace coresql
