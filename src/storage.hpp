#pragma once
#include "state.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <chrono>

#ifndef CORESQL_MAX_ENCODED_MIB
#define CORESQL_MAX_ENCODED_MIB 1024
#endif
namespace coresql::detail {
using MaintenanceClock = std::chrono::steady_clock;
inline std::uint64_t elapsed_ns(MaintenanceClock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(MaintenanceClock::now() - start).count());
}
class MaintenanceTimer {
    std::atomic<std::uint64_t>& counter_;
    MaintenanceClock::time_point start_ = MaintenanceClock::now();

public:
    explicit MaintenanceTimer(std::atomic<std::uint64_t>& counter) : counter_(counter) {}
    ~MaintenanceTimer() { counter_.fetch_add(elapsed_ns(start_), std::memory_order_relaxed); }
};
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
    const std::function<bool(std::uint64_t, const std::function<void(ByteView)>&)>& reuse = {},
    const std::function<void(std::uint64_t identity, std::uint64_t offset, std::uint64_t length)>& note = {});
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
        std::shared_ptr<const CheckpointImages> reuse_;
        std::shared_ptr<CheckpointImages> built_;
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
    std::atomic<std::uint64_t> checkpoint_prepare_ns{0};
    std::atomic<std::uint64_t> checkpoint_encode_ns{0};
    std::atomic<std::uint64_t> checkpoint_catchup_ns{0};
    std::atomic<std::uint64_t> checkpoint_publish_ns{0};
    std::atomic<std::uint64_t> checkpoint_capture_wait_ns{0};
    std::atomic<std::uint64_t> checkpoint_publish_wait_ns{0};
    std::atomic<std::uint64_t> checkpoint_sync_ns{0};
    std::atomic<std::uint64_t> checkpoint_directory_ns{0};
    std::atomic<std::uint64_t> checkpoint_catchup_bytes{0};
    std::atomic<std::uint64_t> checkpoint_catchup_passes{0};
    std::atomic<std::uint64_t> background_checkpoints{0};
    std::atomic<std::uint64_t> superseded_checkpoints{0};
    std::atomic<std::uint64_t> automatic_checkpoints{0};
    std::atomic<std::uint64_t> checkpoint_reused_chunks{0};
    std::atomic<std::uint64_t> checkpoint_encoded_chunks{0};
    std::atomic<bool> failed{false};

private:
    int fd_ = -1;
    std::filesystem::path path_;
    void append(int fd, ByteView);
    void append_checkpoint(int fd, const State&, const CheckpointImages*, int reuse_fd,
                           CheckpointImages* built);
    void retain_images(std::shared_ptr<const CheckpointImages> built);
    void checkpoint_locked(const State&);
    void copy_tail(Checkpoint&, std::int64_t end);
    std::mutex io_mutex_;
    std::uint64_t generation_ = 0;
    std::int64_t committed_end_ = 8;
    int image_fd_ = -1;
    std::shared_ptr<const CheckpointImages> images_;
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
