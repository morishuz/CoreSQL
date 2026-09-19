#include "storage.hpp"
#include "query.hpp"

namespace coresql {
namespace {
[[noreturn]] void fail(ErrorCode code, const std::string& message) {
    throw Error(code, message);
}
Schema describe(const detail::Tables& tables) {
    Schema result;
    for (const auto& [name, table] : tables)
        result.emplace(name, table->columns);
    return result;
}
} // namespace

#ifdef CORESQL_TESTING
thread_local detail::QueryCounters* detail::active_query_counters = nullptr;
std::size_t& detail::retained_matches() {
    static thread_local std::size_t count = 0;
    return count;
}
std::size_t& detail::visited_chunks() {
    static thread_local std::size_t count = 0;
    return count;
}
#endif
void detail::refresh(Chunk& chunk) {
    chunk.payload_bytes = 0;
    chunk.encoded_bytes = chunk.rows.size() * 8;
    for (const auto& row : chunk.rows)
        for (const auto& value : row) {
            std::size_t bytes = 8, prefix = 0;
            if (const auto* v = std::get_if<std::string>(&value)) {
                bytes = v->size();
                prefix = 8;
            } else if (const auto* v = std::get_if<Opaque>(&value)) {
                bytes = v->bytes().size();
                prefix = 8;
            } else if (const auto* v = std::get_if<Compact>(&value)) {
                bytes = v->bytes().size();
            }
            if (is_null(value))
                bytes = 0;
            chunk.payload_bytes += bytes;
            chunk.encoded_bytes += bytes + prefix + 8;
        }
}
void detail::refresh(Table& table) {
    table.row_count = table.payload_bytes = 0;
    for (const auto& [id, chunk] : table.chunks) {
        (void)id;
        table.row_count += chunk->rows.size();
        table.payload_bytes += chunk->payload_bytes;
    }
}
Stats detail::measure(const State& state) {
    Stats result;
    result.tables = state.tables.size();
    for (const auto& [name, table] : state.tables) {
        (void)name;
        result.rows += table->row_count;
        result.stored_payload_bytes += table->payload_bytes;
    }
    result.peak_stored_payload_bytes =
        std::max(state.stats.peak_stored_payload_bytes, result.stored_payload_bytes);
    return result;
}

Database::Database(Registry registry)
    : owner_(std::make_shared<detail::Owner>(
          detail::Owner{std::move(registry), std::make_shared<const detail::State>(), {}})) {
}
Database::~Database() = default;
Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

bool Database::persistent() const {
    detail::healthy(owner_);
    return bool(owner_->storage);
}
StorageStats Database::storage_stats() const {
    detail::healthy(owner_);
    if (!owner_->storage)
        return {};
    return {owner_->storage->bytes_written, owner_->storage->checkpoints};
}
void Database::checkpoint() {
    detail::healthy(owner_);
    if (!owner_->storage)
        fail(ErrorCode::state, "Checkpoint requires persistent storage");
    owner_->storage->checkpoint(*owner_->current);
}
Transaction Database::begin() {
    detail::healthy(owner_);
    return Transaction(owner_);
}
Result Database::query(const Query& query) const {
    detail::healthy(owner_);
    auto snapshot = owner_->current;
    return detail::execution::run(snapshot->tables, query, owner_->registry);
}
Stats Database::stats() const {
    detail::healthy(owner_);
    return owner_->current->stats;
}
Schema Database::schema() const {
    detail::healthy(owner_);
    return describe(owner_->current->tables);
}

Result Transaction::query(const Query& query) const {
    auto owner = active();
    return detail::execution::run(staged_->tables, query, owner->registry);
}
Schema Transaction::schema() const {
    active();
    return describe(staged_->tables);
}

} // namespace coresql
