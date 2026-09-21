#include "worker.hpp"
#include <set>
namespace landmarks {
namespace {
void validate(const std::string& model, const std::string& frame, double x, double y,
              const std::array<float, dimensions>& descriptor) {
    if (model.empty() || model.size() > 64 || frame.empty() || frame.size() > 64 || !std::isfinite(x) ||
        !std::isfinite(y) ||
        !std::all_of(descriptor.begin(), descriptor.end(), [](float value) { return std::isfinite(value); }))
        throw Error(ErrorCode::constraint, "Invalid landmark model/frame, coordinates or descriptor");
}
sql::Result stage_ingest(sql::Connection& c, std::span<const Observation> batch, std::size_t& count,
                         std::size_t maximum) {
    static const sql::Statement exists("SELECT id FROM observations WHERE id=?");
    static const sql::Statement replace(
        "INSERT INTO observations VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
        "model=excluded.model,frame=excluded.frame,x=excluded.x,y=excluded.y,"
        "seen=excluded.seen,descriptor=excluded.descriptor");
    std::set<std::int64_t> added;
    for (const auto& item : batch)
        if (c.execute(exists, Row{item.id}).rows.empty())
            added.insert(item.id);
    if (added.size() > maximum - count)
        throw Error(ErrorCode::resource, "Landmark capacity reached; no observations were changed");
    for (const auto& item : batch)
        c.execute(replace, Row{item.id, item.model, item.frame, item.x, item.y, item.seen,
                               vectors::value(item.descriptor, dimensions)});
    count += added.size();
    sql::Result result;
    result.changes = batch.size();
    return result;
}
sql::Result search(sql::Connection& c, const Search& request) {
    static const sql::Statement query(
        "SELECT id,vector_squared_l2(descriptor,?1) AS distance FROM observations "
        "WHERE model=?2 AND frame=?3 AND x BETWEEN ?4 AND ?5 AND y BETWEEN ?6 AND ?7 "
        "ORDER BY distance,id LIMIT 5");
    auto options = request.options;
    // Always bound query work, even when the caller does not supply a limit.
    options.max_work = std::min(options.max_work, std::size_t{10000000});
    return c.query(query,
                   Row{vectors::value(request.descriptor, dimensions), request.model, request.frame,
                       request.x - request.radius, request.x + request.radius, request.y - request.radius,
                       request.y + request.radius},
                   options);
}
sql::Result erase(sql::Connection& c, std::span<const std::int64_t> ids, std::size_t& count,
                  double& commit_ms) {
    static const sql::Statement statement("DELETE FROM observations WHERE id=?");
    sql::Result result;
    static const sql::Statement begin("BEGIN"), commit("COMMIT"), rollback("ROLLBACK");
    c.execute(begin);
    try {
        for (auto id : ids)
            result.changes += c.execute(statement, Row{id}).changes;
        const auto start = std::chrono::steady_clock::now();
        c.execute(commit);
        commit_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    } catch (...) {
        if (c.in_transaction())
            c.execute(rollback);
        throw;
    }
    count -= result.changes;
    return result;
}
} // namespace
Worker::Worker(const std::filesystem::path& path, WorkerLimits limits) : limits_(limits) {
    if (!limits_.landmarks || limits_.landmarks > 100000 || !limits_.queued_requests ||
        limits_.queued_requests > 64 || !limits_.batch_requests || limits_.batch_requests > 64 ||
        limits_.batch_observations < 32 || limits_.batch_observations > 256 ||
        limits_.batch_wait.count() < 0 || limits_.batch_wait > std::chrono::milliseconds(10) ||
        limits_.checkpoint_max_delay.count() < 0)
        throw Error(ErrorCode::constraint, "Invalid worker capacity, queue, batch or maintenance limits");
    std::promise<void> startup;
    auto initialized = startup.get_future();
    thread_ =
        std::jthread([this, path, startup = std::move(startup)]() mutable { run(path, std::move(startup)); });
    initialized.get();
}
Worker::~Worker() {
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    ready_.notify_one();
    thread_.join();
}
std::optional<std::future<sql::Result>> Worker::try_ingest(std::span<const Observation> batch) {
    if (batch.empty() || batch.size() > 32)
        throw Error(ErrorCode::constraint, "Ingest batch must contain 1..32 observations");
    for (const auto& item : batch) {
        validate(item.model, item.frame, item.x, item.y, item.descriptor);
        if (item.id < 0 || item.seen < 0)
            throw Error(ErrorCode::constraint, "Negative observation ID or sequence");
    }
    return enqueue(std::vector<Observation>(batch.begin(), batch.end()));
}
std::optional<std::future<sql::Result>> Worker::try_search(const Search& search) {
    validate(search.model, search.frame, search.x, search.y, search.descriptor);
    if (!(search.radius >= 0) || !std::isfinite(search.x - search.radius) ||
        !std::isfinite(search.x + search.radius) || !std::isfinite(search.y - search.radius) ||
        !std::isfinite(search.y + search.radius))
        throw Error(ErrorCode::constraint, "Invalid search radius");
    return enqueue(search);
}
std::optional<std::future<sql::Result>> Worker::try_erase(std::span<const std::int64_t> ids) {
    if (ids.empty() || ids.size() > 32 || std::any_of(ids.begin(), ids.end(), [](auto id) { return id < 0; }))
        throw Error(ErrorCode::constraint, "Erase requires 1..32 nonnegative IDs");
    return enqueue(std::vector<std::int64_t>(ids.begin(), ids.end()));
}
std::optional<std::future<sql::Result>> Worker::try_checkpoint() {
    return enqueue(Checkpoint{});
}
std::optional<std::future<sql::Result>> Worker::enqueue(Operation operation) {
    std::lock_guard lock(mutex_);
    if (failure_)
        std::rethrow_exception(failure_);
    if (closing_ || queue_.size() == limits_.queued_requests) {
        ++stats_.rejected;
        return {};
    }
    Request request{std::move(operation), {}};
    auto future = request.completion.get_future();
    queue_.push_back(std::move(request));
    ++stats_.accepted;
    stats_.peak_queue = std::max(stats_.peak_queue, queue_.size());
    ready_.notify_one();
    return future;
}
void Worker::record(LatencySample sample, bool request) {
    std::lock_guard lock(mutex_);
    sample.sequence = ++stats_.last_sequence;
    samples_[sample_next_] = sample;
    sample_next_ = (sample_next_ + 1) % samples_.size();
    sample_count_ = std::min(sample_count_ + 1, samples_.size());
    if (request)
        ++stats_.completed;
    if (!sample.success)
        ++stats_.failures;
}
WorkerStats Worker::stats(std::uint64_t after_sequence) const {
    std::lock_guard lock(mutex_);
    auto result = stats_;
    result.stopped = closing_;
    result.samples.reserve(sample_count_);
    const auto first = (sample_next_ + samples_.size() - sample_count_) % samples_.size();
    for (std::size_t i = 0; i < sample_count_; ++i) {
        const auto& sample = samples_[(first + i) % samples_.size()];
        if (sample.sequence > after_sequence)
            result.samples.push_back(sample);
    }
    return result;
}
void Worker::run(const std::filesystem::path& path, std::promise<void> startup) {
    bool started = false;
    try {
        auto registry = landmarks::registry();
        auto db = Database::open(path, registry);
        sql::Connection c(db, registry);
        c.enable_query_cache(true);
        const auto existing = db.schema();
        if (!existing.empty() && (existing.size() != 1 || !existing.contains("observations")))
            throw Error(ErrorCode::schema, "Worker requires a dedicated observations file");
        c.execute("CREATE TABLE IF NOT EXISTS observations(id INTEGER PRIMARY KEY CHECK(id>=0), model TEXT "
                  "NOT NULL, "
                  "frame TEXT NOT NULL,x REAL NOT NULL,y REAL NOT NULL,seen INTEGER NOT NULL "
                  "CHECK(seen>=0),descriptor "
                  "VECTOR(128) NOT NULL)");
        const auto count_result = c.execute("SELECT count(*) FROM observations");
        auto count = static_cast<std::size_t>(std::get<std::int64_t>(count_result.rows[0][0]));
        if (count > limits_.landmarks)
            throw Error(ErrorCode::resource, "Existing map exceeds worker landmark capacity");
        const auto schema = db.schema();
        const std::array<std::string, 7> names{"id", "model", "frame", "x", "y", "seen", "descriptor"};
        const std::array<Type, 7> types{
            integer(), text(), text(), real(), real(), integer(), vectors::type(dimensions)};
        if (schema.size() != 1 || schema.at("observations").size() != names.size())
            throw Error(ErrorCode::schema, "Worker requires its dedicated observations schema");
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto& column = schema.at("observations")[i];
            if (column.name != names[i] || column.type != types[i] || column.nullable ||
                column.primary_key != (i == 0))
                throw Error(ErrorCode::schema, "Unexpected observations column");
        }
        c.query_each(sql::Statement("SELECT id,model,frame,seen,x,y FROM observations"), [](auto row) {
            if (std::get<std::int64_t>(row[0]) < 0 || std::get<std::int64_t>(row[3]) < 0 ||
                std::get<std::string>(row[1]).empty() || std::get<std::string>(row[1]).size() > 64 ||
                std::get<std::string>(row[2]).empty() || std::get<std::string>(row[2]).size() > 64 ||
                !std::isfinite(std::get<double>(row[4])) || !std::isfinite(std::get<double>(row[5])))
                throw Error(ErrorCode::constraint, "Stored observation exceeds worker contract");
            return true;
        });
        const IndexDefinition required{"observations_local", {"model", "frame", "y", "x"}};
        const auto indexes = db.begin().indexes("observations");
        auto index = std::find_if(indexes.begin(), indexes.end(),
                                  [&](const auto& i) { return i.name == required.name; });
        if (index != indexes.end() &&
            (index->columns != required.columns || index->unique ||
             std::any_of(index->descending.begin(), index->descending.end(), [](bool d) { return d; })))
            throw Error(ErrorCode::schema, "Unexpected observations_local index");
        c.execute("CREATE INDEX IF NOT EXISTS observations_local ON observations(model,frame,y,x)");
        db.checkpoint();
        using Clock = std::chrono::steady_clock;
        auto milliseconds = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        std::size_t mutations = 0, since_checkpoint = 0;
        std::optional<Clock::time_point> due;
        auto checkpoint = [&](bool sample = true) {
            const auto begin = Clock::now();
            try {
                db.checkpoint();
            } catch (...) {
                if (sample)
                    record({LatencySample::checkpoint, 0, milliseconds(Clock::now() - begin), 0, false},
                           false);
                throw;
            }
            if (sample)
                record({LatencySample::checkpoint, 0, milliseconds(Clock::now() - begin), 0, true}, false);
            since_checkpoint = 0;
            due.reset();
            std::lock_guard lock(mutex_);
            ++stats_.checkpoints;
            stats_.maintenance_due = false;
        };
        started = true;
        startup.set_value();
        for (;;) {
            std::vector<Request> batch;
            bool maintenance = false;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return closing_ || !queue_.empty() || due.has_value(); });
                maintenance = due && (queue_.empty() || Clock::now() - *due >= limits_.checkpoint_max_delay);
                if (!maintenance && queue_.empty())
                    break;
                if (!maintenance) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                    if (auto* ingest = std::get_if<std::vector<Observation>>(&batch.front().operation)) {
                        std::size_t count = ingest->size();
                        const auto deadline = batch.front().accepted + limits_.batch_wait;
                        for (;;) {
                            if (batch.size() >= limits_.batch_requests)
                                break;
                            if (queue_.empty() && !closing_)
                                ready_.wait_until(lock, deadline,
                                                  [&] { return closing_ || !queue_.empty(); });
                            if (queue_.empty())
                                break;
                            auto* next = std::get_if<std::vector<Observation>>(&queue_.front().operation);
                            if (!next || count + next->size() > limits_.batch_observations)
                                break;
                            count += next->size();
                            batch.push_back(std::move(queue_.front()));
                            queue_.pop_front();
                        }
                    }
                }
            }
            if (maintenance) {
                checkpoint();
                continue;
            }
            const auto begin = Clock::now();
            std::size_t successful = 0;
            if (std::holds_alternative<std::vector<Observation>>(batch[0].operation)) {
                struct Outcome {
                    sql::Result result;
                    std::exception_ptr error;
                    double execution = 0;
                };
                std::vector<Outcome> outcomes(batch.size());
                static const sql::Statement start("BEGIN"), commit("COMMIT"), rollback("ROLLBACK"),
                    save("SAVEPOINT worker_request"), undo("ROLLBACK TO worker_request"),
                    release("RELEASE worker_request");
                const auto original_count = count;
                double commit_ms = 0;
                try {
                    c.execute(start);
                    for (std::size_t i = 0; i < batch.size(); ++i) {
                        const auto begin_request = Clock::now();
                        const auto before = count;
                        c.execute(save);
                        try {
                            outcomes[i].result =
                                stage_ingest(c, std::get<std::vector<Observation>>(batch[i].operation), count,
                                             limits_.landmarks);
                            c.execute(release);
                            ++successful;
                        } catch (...) {
                            outcomes[i].error = std::current_exception();
                            c.execute(undo);
                            c.execute(release);
                            count = before;
                        }
                        outcomes[i].execution = milliseconds(Clock::now() - begin_request);
                    }
                    const auto commit_begin = Clock::now();
                    c.execute(successful ? commit : rollback);
                    commit_ms = milliseconds(Clock::now() - commit_begin);
                    if (successful) {
                        std::lock_guard lock(mutex_);
                        ++stats_.committed_batches;
                    }
                } catch (...) {
                    const auto error = std::current_exception();
                    if (c.in_transaction()) {
                        try {
                            c.execute(rollback);
                        } catch (...) {
                        }
                    }
                    count = original_count;
                    successful = 0;
                    for (auto& outcome : outcomes)
                        if (!outcome.error)
                            outcome.error = error;
                }
                // Complete only after the whole batch's durable publication.
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    auto& outcome = outcomes[i];
                    record({LatencySample::ingest, milliseconds(begin - batch[i].accepted), outcome.execution,
                            commit_ms, !outcome.error, milliseconds(Clock::now() - batch[i].accepted)});
                    if (outcome.error)
                        batch[i].completion.set_exception(outcome.error);
                    else
                        batch[i].completion.set_value(std::move(outcome.result));
                }
            } else {
                auto& request = batch[0];
                const auto kind = std::holds_alternative<Search>(request.operation) ? LatencySample::search
                                  : std::holds_alternative<Checkpoint>(request.operation)
                                      ? LatencySample::checkpoint
                                      : LatencySample::erase;
                double commit_ms = 0;
                try {
                    sql::Result result;
                    if (kind == LatencySample::search)
                        result = search(c, std::get<Search>(request.operation));
                    else if (kind == LatencySample::erase) {
                        result = erase(c, std::get<std::vector<std::int64_t>>(request.operation), count,
                                       commit_ms);
                        successful = 1;
                        std::lock_guard lock(mutex_);
                        ++stats_.committed_batches;
                    } else
                        checkpoint(false);
                    record({kind, milliseconds(begin - request.accepted),
                            milliseconds(Clock::now() - begin) - commit_ms, commit_ms, true,
                            milliseconds(Clock::now() - request.accepted)});
                    request.completion.set_value(std::move(result));
                } catch (...) {
                    record({kind, milliseconds(begin - request.accepted), milliseconds(Clock::now() - begin),
                            commit_ms, false, milliseconds(Clock::now() - request.accepted)});
                    request.completion.set_exception(std::current_exception());
                    if (kind == LatencySample::checkpoint)
                        throw;
                }
            }
            mutations += successful;
            since_checkpoint += successful;
            if (limits_.checkpoint_every && since_checkpoint >= limits_.checkpoint_every && !due) {
                due = Clock::now();
                std::lock_guard lock(mutex_);
                stats_.maintenance_due = true;
            }
        }
        if (mutations && since_checkpoint)
            checkpoint();
    } catch (...) {
        if (!started)
            startup.set_exception(std::current_exception());
        else {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
            closing_ = true;
            for (auto& request : queue_) {
                ++stats_.completed;
                ++stats_.failures;
                request.completion.set_exception(failure_);
            }
            queue_.clear();
        }
    }
}
} // namespace landmarks
