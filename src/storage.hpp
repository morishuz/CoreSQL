#pragma once
#include "state.hpp"
#include <atomic>
#include <functional>
#include <mutex>

#ifndef CORESQL_MAX_ENCODED_MIB
#define CORESQL_MAX_ENCODED_MIB 1024
#endif
namespace coresql::detail {
inline constexpr std::size_t max_encoded_bytes = std::size_t{CORESQL_MAX_ENCODED_MIB} * 1024 * 1024;
Bytes encode(const State&);
void validate_snapshot(ByteView);
void refresh(Chunk&);
void refresh(Table&);
Stats measure(const State&);
std::size_t checkpoint_size(const State&);
Bytes encode_changes(const State& base, const State& next);
// Emits a complete change record without allocating the full encoded state.
// reuse, when set, may emit an unchanged chunk record and return true.
// note observes each emitted chunk record at its payload-relative offset.
void encode_checkpoint(
    const State&, const std::function<void(ByteView)>& sink,
    const std::function<bool(const Chunk&, const std::function<void(ByteView)>&)>& reuse = {},
    const std::function<void(const Chunk&, std::uint64_t offset, std::uint64_t length)>& note = {});
class Pager;
struct CheckpointImages;
State apply_changes(const State& base, ByteView, const Registry&, const std::shared_ptr<Pager>& = {});
class DurableStore {
public:
    class Checkpoint {
    public:
        ~Checkpoint();
        Checkpoint(const Checkpoint&) = delete;
        Checkpoint& operator=(const Checkpoint&) = delete;

    private:
        friend class DurableStore;
        explicit Checkpoint(DurableStore&);
        DurableStore* owner_;
        int source_ = -1, fresh_ = -1;
        std::uint64_t generation_ = 0;
        std::int64_t copied_ = 0;
        bool ready_ = false;
        std::filesystem::path temporary_;
        std::unique_ptr<CheckpointImages> reuse_, built_;
    };
    explicit DurableStore(const std::filesystem::path&, bool create_only = false);
    ~DurableStore();
    DurableStore(const DurableStore&) = delete;
    DurableStore& operator=(const DurableStore&) = delete;
    State recover(const Registry&, const std::shared_ptr<Pager>& = {});
    void commit(const State& base, const State& next);
    void checkpoint(const State&);
    // Capture and publish run under the caller's writer lock; encoding runs outside it.
    // The supplied state must be the committed snapshot captured with prepare.
    std::unique_ptr<Checkpoint> prepare_checkpoint();
    void write_checkpoint(Checkpoint&, const State&);
    // Check under the writer lock; false requests another unlocked catch-up pass.
    bool ready_to_publish(const Checkpoint&);
    // Fingerprint describes only the recovered log, before any new commits.
    std::uint64_t recovery_fingerprint() const { return recovery_fingerprint_; }
    const std::filesystem::path& path() const { return path_; }
    // False means a newer synchronous checkpoint already superseded this one.
    bool publish_checkpoint(Checkpoint&);
    std::atomic<std::uint64_t> bytes_written{0}, checkpoints{0};
    std::atomic<bool> failed{false};

private:
    int fd_ = -1;
    std::filesystem::path path_;
    void append(int fd, ByteView);
    void append_checkpoint(int fd, const State&, const CheckpointImages*, int reuse_fd,
                           CheckpointImages* built);
    void retain_images(std::unique_ptr<CheckpointImages> built);
    void checkpoint_locked(const State&);
    void copy_tail(Checkpoint&, std::int64_t end);
    std::mutex io_mutex_;
    std::uint64_t generation_ = 0;
    std::int64_t committed_end_ = 8;
    int image_fd_ = -1;
    std::unique_ptr<CheckpointImages> images_;
    std::uint64_t recovery_fingerprint_ = 14695981039346656037ULL;
    bool background_active_ = false;
};
inline void healthy(const std::shared_ptr<Owner>& owner) {
    if (!owner || (owner->storage && owner->storage->failed))
        throw Error(ErrorCode::state,
                    "Database unavailable; close and reopen after an uncertain I/O outcome");
}
#ifdef CORESQL_TESTING
// Private fault seam: absent from production builds with BUILD_TESTING=OFF.
using StorageHook = void (*)(const char* stage);
void storage_hook(StorageHook);
#endif
} // namespace coresql::detail
