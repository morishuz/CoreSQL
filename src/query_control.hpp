#pragma once
#include "coresql/core.hpp"

namespace coresql::detail::execution {
struct QueryUsage {
    std::size_t work = 0, buffer = 0, peak_buffer = 0;
};
class QueryControl;
inline thread_local QueryControl* active_control = nullptr;
class QueryControl {
    const QueryOptions& options;
    QueryControl* previous;
    QueryControl* parent;
    QueryUsage local;
    QueryUsage* usage;

public:
    explicit QueryControl(const QueryOptions& value, QueryUsage* retained = nullptr)
        : options(value), previous(active_control), parent(previous), usage(retained ? retained : &local) {
        // fetch() and next() share one cursor budget, without charging it twice.
        if (parent && parent->usage == usage)
            parent = parent->parent;
        check();
        if (retained || options.cancellation.stop_possible() || options.deadline ||
            options.max_work != std::numeric_limits<std::size_t>::max() ||
            options.max_buffer_bytes != std::numeric_limits<std::size_t>::max())
            active_control = this;
    }
    ~QueryControl() { active_control = previous; }
    QueryControl(const QueryControl&) = delete;
    QueryControl& operator=(const QueryControl&) = delete;
    void check() const {
        if (parent)
            parent->check();
        if (options.cancellation.stop_requested())
            throw Error(ErrorCode::cancelled, "Query cancelled");
        if (options.deadline && std::chrono::steady_clock::now() >= *options.deadline)
            throw Error(ErrorCode::cancelled, "Query deadline exceeded");
    }
    void step() {
        if (parent)
            parent->step();
        if (usage->work == options.max_work)
            throw Error(ErrorCode::resource, "Query work limit exceeded");
        ++usage->work;
        if ((usage->work & 255) == 0)
            check();
    }
    void reserve(std::size_t bytes) {
        if (bytes > options.max_buffer_bytes - usage->buffer)
            throw Error(ErrorCode::resource, "Query buffer limit exceeded");
        if (parent)
            parent->reserve(bytes);
        usage->buffer += bytes;
        usage->peak_buffer = std::max(usage->peak_buffer, usage->buffer);
    }
    void release(std::size_t bytes) noexcept {
        usage->buffer -= bytes;
        if (parent)
            parent->release(bytes);
    }
};
inline void query_step() {
    if (active_control)
        active_control->step();
}
inline std::size_t value_buffer_bytes(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value))
        return text->size();
    if (const auto* opaque = std::get_if<Opaque>(&value))
        return opaque->bytes().size();
    return 0;
}
inline std::size_t row_buffer_bytes(std::span<const Value> row) {
    std::size_t bytes = row.size() * sizeof(Value);
    for (const auto& value : row) {
        const auto payload = value_buffer_bytes(value);
        if (payload > std::numeric_limits<std::size_t>::max() - bytes)
            throw Error(ErrorCode::resource, "Query buffer size overflow");
        bytes += payload;
    }
    return bytes;
}
// Accounts retained row/key payloads and conservative container-node allowances.
// Extension-private allocations and immutable database snapshots are excluded.
class QueryBuffer {
    QueryControl* control_ = active_control;
    std::size_t bytes_ = 0;

public:
    QueryBuffer() = default;
    QueryBuffer(const QueryBuffer&) = delete;
    QueryBuffer& operator=(const QueryBuffer&) = delete;
    ~QueryBuffer() {
        if (control_)
            control_->release(bytes_);
    }
    void add(std::size_t bytes) {
        if (!control_)
            return;
        if (bytes > std::numeric_limits<std::size_t>::max() - bytes_)
            throw Error(ErrorCode::resource, "Query buffer size overflow");
        control_->reserve(bytes);
        bytes_ += bytes;
    }
    bool active() const { return control_ != nullptr; }
    void add_row(std::span<const Value> row, std::size_t overhead = 0, std::size_t copies = 1) {
        if (!control_)
            return;
        const auto bytes = row_buffer_bytes(row);
        if (copies && bytes > (std::numeric_limits<std::size_t>::max() - overhead) / copies)
            throw Error(ErrorCode::resource, "Query buffer size overflow");
        add(bytes * copies + overhead);
    }
    void remove(std::size_t bytes) noexcept {
        if (control_) {
            control_->release(bytes);
            bytes_ -= bytes;
        }
    }
};
} // namespace coresql::detail::execution
