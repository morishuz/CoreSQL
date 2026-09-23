#include "storage.hpp"
#include "query.hpp"

namespace coresql {
ReadSnapshot::ReadSnapshot(std::shared_ptr<const detail::State> state,
                           std::shared_ptr<const Registry> registry)
    : state_(std::move(state)), registry_(std::move(registry)), created_(std::chrono::steady_clock::now()) {
}
ReadSnapshot Database::snapshot() const {
    detail::healthy(owner_);
    if (!owner_->options.concurrent_reads)
        throw Error(ErrorCode::state,
                    "Read snapshots require OpenOptions::concurrent_reads and concurrent-safe callbacks");
    return ReadSnapshot(owner_->capture(), owner_->read_registry);
}
Result ReadSnapshot::query(const Query& query, const QueryOptions& options) const {
    detail::execution::QueryControl control(options);
    auto result = detail::execution::run(state_->tables, query, *registry_);
    control.check();
    return result;
}
StreamResult ReadSnapshot::query_each(const Query& query, const RowVisitor& visit,
                                      const QueryOptions& options) const {
    detail::execution::QueryControl control(options);
    auto result = detail::execution::stream(state_->tables, query, *registry_, visit);
    control.check();
    return result;
}
QueryCursor ReadSnapshot::cursor(const Query& query, const QueryOptions& options) const {
    return detail::execution::make_cursor(state_->tables, *registry_, query, options);
}
QueryCursor Database::cursor(const Query& query, const QueryOptions& options) const {
    detail::healthy(owner_);
    return detail::execution::make_cursor(owner_->capture()->tables, owner_->registry, query, options);
}
QueryCursor Transaction::cursor(const Query& query, const QueryOptions& options) const {
    const auto owner = active();
    return detail::execution::make_cursor(staged_->tables, owner->registry, query, options);
}
Schema ReadSnapshot::schema() const {
    Schema result;
    for (const auto& [name, table] : state_->tables)
        result.emplace(name, table->columns);
    return result;
}
Stats ReadSnapshot::stats() const {
    return state_->stats;
}
const Registry& ReadSnapshot::registry() const {
    return *registry_;
}
std::chrono::steady_clock::duration ReadSnapshot::age() const {
    return std::chrono::steady_clock::now() - created_;
}
std::future<void> Database::checkpoint_async() {
    detail::healthy(owner_);
    if (!owner_->options.concurrent_reads)
        throw Error(ErrorCode::state, "Background checkpoints require concurrent-safe callbacks");
    auto owner = owner_;
    auto store = owner->storage;
    if (!store)
        throw Error(ErrorCode::state, "Checkpoint requires persistent storage");
    std::shared_ptr<const detail::State> state;
    std::unique_ptr<detail::DurableStore::Checkpoint> work;
    {
        const auto start = detail::MaintenanceClock::now();
        std::lock_guard lock(owner->commit_mutex);
        store->checkpoint_capture_wait_ns += detail::elapsed_ns(start);
        detail::healthy(owner);
        state = owner->capture();
        work = store->prepare_checkpoint();
    }
    return std::async(std::launch::async, [owner = std::move(owner), store = std::move(store),
                                           state = std::move(state), work = std::move(work)]() mutable {
        // Destroy the reservation before the future becomes ready,
        // including exceptions, while the retained store is alive.
        auto checkpoint = std::move(work);
        for (;;) {
            store->write_checkpoint(*checkpoint, *state);
            const auto start = detail::MaintenanceClock::now();
            std::lock_guard lock(owner->commit_mutex);
            store->checkpoint_publish_wait_ns += detail::elapsed_ns(start);
            detail::healthy(owner);
            if (!store->ready_to_publish(*checkpoint))
                continue;
            (void)store->publish_checkpoint(*checkpoint);
            break;
        }
    });
}
} // namespace coresql
