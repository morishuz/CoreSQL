#pragma once
#include "coresql/core.hpp"

namespace coresql::detail::execution {
class QueryControl;
inline thread_local QueryControl* active_control = nullptr;
class QueryControl {
    const QueryOptions& options;
    QueryControl* parent;
    std::size_t used = 0;

public:
    explicit QueryControl(const QueryOptions& value) : options(value), parent(active_control) {
        check();
        if (options.cancellation.stop_possible() || options.deadline ||
            options.max_work != std::numeric_limits<std::size_t>::max())
            active_control = this;
    }
    ~QueryControl() { active_control = parent; }
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
        if (used == options.max_work)
            throw Error(ErrorCode::resource, "Query work limit exceeded");
        ++used;
        if ((used & 255) == 0)
            check();
    }
};
inline void query_step() {
    if (active_control)
        active_control->step();
}
} // namespace coresql::detail::execution
