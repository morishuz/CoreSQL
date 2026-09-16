#pragma once
#include "state.hpp"

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
State apply_changes(const State& base, ByteView, const Registry&);
class DurableStore {
public:
    explicit DurableStore(const std::filesystem::path&, bool create_only = false);
    ~DurableStore();
    DurableStore(const DurableStore&) = delete;
    DurableStore& operator=(const DurableStore&) = delete;
    State recover(const Registry&);
    void commit(const State& base, const State& next);
    void checkpoint(const State&);
    std::uint64_t bytes_written = 0, checkpoints = 0;

    bool failed = false;
private:
    int fd_ = -1;
    std::filesystem::path path_;
    void append(int fd, ByteView);

};
inline void healthy(const std::shared_ptr<Owner>& owner) {
    if (!owner || (owner->storage && owner->storage->failed))
        throw Error(ErrorCode::state, "Database unavailable; close and reopen after an uncertain I/O outcome");
}
#ifdef CORESQL_TESTING
// Private fault seam: absent from production builds with BUILD_TESTING=OFF.
using StorageHook = void(*)(const char* stage);
void storage_hook(StorageHook);
#endif
}
