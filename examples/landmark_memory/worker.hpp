#pragma once
#include "workload.hpp"
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace landmarks {
struct Observation {
    std::int64_t id = 0, seen = 0;
    std::string model, frame;
    double x = 0, y = 0;
    std::array<float, dimensions> descriptor{};
};
struct Search {
    std::string model, frame;
    double x = 0, y = 0, radius = 10;
    std::array<float, dimensions> descriptor{};
    QueryOptions options;
};
struct WorkerLimits {
    std::size_t landmarks = 100000, queued_requests = 32;
    std::size_t batch_requests = 16;           // Set to one to disable coalescing.
    std::size_t batch_observations = 128;      // 32..256; consecutive ingest requests only.
    std::chrono::microseconds batch_wait{500}; // 0..10000, measured from first acceptance.
    std::size_t checkpoint_every = 256;        // Successful mutation requests; zero means explicit only.
    std::chrono::milliseconds checkpoint_max_delay{1000};
};
struct LatencySample {
    enum Kind { ingest, search, erase, checkpoint } kind;
    double queue_ms = 0, execute_ms = 0, commit_ms = 0;
    bool success = false;
    double total_ms = 0; // Acceptance through completion; includes time spent in shared batches.
    std::uint64_t sequence = 0;
};
struct WorkerStats {
    std::size_t accepted = 0, rejected = 0, completed = 0, failures = 0;
    std::size_t committed_batches = 0, checkpoints = 0, peak_queue = 0;
    std::uint64_t last_sequence = 0;
    bool maintenance_due = false, stopped = false;
    // Most recent 4096 completed operations, including explicit/idle checkpoints.
    std::vector<LatencySample> samples;
};
// Application example: exactly one thread owns the database and SQL connection.
// Accepted writes are acknowledged only after synchronized commit. Destruction
// drains accepted work and joins; it is not a deadline-bounded shutdown operation.
class Worker {
public:
    explicit Worker(const std::filesystem::path&, WorkerLimits = {});
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    // Empty means backpressure: no request was accepted and input is unchanged.
    // A terminal worker failure is rethrown on subsequent submissions.
    std::optional<std::future<sql::Result>> try_ingest(std::span<const Observation>);
    std::optional<std::future<sql::Result>> try_search(const Search&);
    std::optional<std::future<sql::Result>> try_erase(std::span<const std::int64_t> ids);
    std::optional<std::future<sql::Result>> try_checkpoint();
    WorkerStats stats(std::uint64_t after_sequence = 0) const;

private:
    struct Checkpoint {};
    using Operation = std::variant<std::vector<Observation>, Search, std::vector<std::int64_t>, Checkpoint>;
    struct Request {
        Operation operation;
        std::promise<sql::Result> completion;
        std::chrono::steady_clock::time_point accepted = std::chrono::steady_clock::now();
    };
    WorkerLimits limits_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Request> queue_;
    bool closing_ = false;
    std::exception_ptr failure_;
    WorkerStats stats_;
    std::array<LatencySample, 4096> samples_{};
    std::size_t sample_count_ = 0, sample_next_ = 0;
    std::jthread thread_;
    std::optional<std::future<sql::Result>> enqueue(Operation);
    void record(LatencySample, bool request = true);
    void run(const std::filesystem::path&, std::promise<void>);
};
} // namespace landmarks
