#pragma once
#include "coresql/sql.hpp"
#include <sqlite3.h>
#include <map>
#include <future>
#include <atomic>
#include <chrono>
#include <memory>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace application {
using namespace coresql;
using Rows = std::vector<Row>;
inline void check(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
inline std::pair<std::uint64_t, std::uint64_t> memory() {
    rusage usage{};
    check(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    check(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
              KERN_SUCCESS,
          "task_info failed");
    return {info.resident_size, static_cast<std::uint64_t>(usage.ru_maxrss)};
#else
    std::ifstream status("/proc/self/statm");
    std::uint64_t total, resident;
    check(bool(status >> total >> resident), "Cannot read process RSS");
    return {resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)),
            static_cast<std::uint64_t>(usage.ru_maxrss) * 1024};
#endif
}
struct Backend {
    bool core, durable, background, adaptive;
    std::uint64_t compacted_bytes = 0;
    std::chrono::steady_clock::time_point last_checkpoint = std::chrono::steady_clock::now();
    std::future<void> maintenance;
    std::uint64_t requested = 0, started = 0, completed = 0, coalesced = 0;
    std::atomic<std::uint64_t> busy_retries{0};
    sqlite3* checkpoint_connection = nullptr;
    bool pending = false;
    std::size_t cache;
    std::filesystem::path path;
    Registry registry;
    std::optional<Database> db;
    std::unique_ptr<sql::Connection> connection;
    sqlite3* sqlite = nullptr;
    std::map<std::string, sql::Statement> parsed;
    struct Finalize {
        void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
    };
    std::map<std::string, std::unique_ptr<sqlite3_stmt, Finalize>> prepared;
    Backend(bool c, bool d, std::size_t bytes, std::filesystem::path file, bool bg = false,
            bool policy = false)
        : core(c), durable(d), background(bg), adaptive(policy), cache(bytes), path(std::move(file)) {
        sql::install(registry);
        open();
    }
    ~Backend() {
        try {
            close();
        } catch (...) { /* Explicit drain reports maintenance errors. */
        }
    }
    void close() {
        std::exception_ptr failure;
        try {
            drain();
        } catch (...) {
            failure = std::current_exception();
        }
        if (checkpoint_connection) {
            sqlite3_close(checkpoint_connection);
            checkpoint_connection = nullptr;
        }
        parsed.clear();
        prepared.clear();
        connection.reset();
        db.reset();
        if (sqlite) {
            sqlite3_close(sqlite);
            sqlite = nullptr;
        }
        if (failure)
            std::rethrow_exception(failure);
    }
    void open() {
        if (core) {
            OpenOptions options{.concurrent_reads = true, .page_cache_bytes = cache};
            if (durable)
                db.emplace(Database::open(path, registry, options));
            else
                db.emplace(registry, options);
            connection = std::make_unique<sql::Connection>(*db, registry);
        } else {
            check(sqlite3_open(durable ? path.c_str() : ":memory:", &sqlite) == SQLITE_OK,
                  "SQLite open failed");
            sqlite3_busy_timeout(sqlite, 5000);
            exec("PRAGMA journal_mode=" + std::string(durable ? "WAL" : "MEMORY"));
            exec("PRAGMA synchronous=FULL");
            exec("PRAGMA fullfsync=ON");
            exec("PRAGMA checkpoint_fullfsync=ON");
            exec("PRAGMA wal_autocheckpoint=0");
            exec("PRAGMA mmap_size=0");
            exec("PRAGMA cache_size=-" + std::to_string((cache ? cache : 64 * 1024 * 1024) / 1024));
            check(exec("PRAGMA synchronous")[0][0] == Value(std::int64_t{2}), "SQLite sync setting");
            check(exec("PRAGMA fullfsync")[0][0] == Value(std::int64_t{1}), "SQLite fullfsync setting");
            check(exec("PRAGMA checkpoint_fullfsync")[0][0] == Value(std::int64_t{1}),
                  "SQLite checkpoint sync setting");
            check(exec("PRAGMA journal_mode")[0][0] == Value(std::string(durable ? "wal" : "memory")),
                  "SQLite journal setting");
            if (background && durable) {
                sqlite3_busy_timeout(sqlite, 5000);
                check(sqlite3_open(path.c_str(), &checkpoint_connection) == SQLITE_OK,
                      "SQLite checkpoint connection failed");
                check(sqlite3_exec(checkpoint_connection,
                                   "PRAGMA synchronous=FULL; PRAGMA fullfsync=ON; "
                                   "PRAGMA checkpoint_fullfsync=ON; PRAGMA wal_autocheckpoint=0;",
                                   nullptr, nullptr, nullptr) == SQLITE_OK,
                      "SQLite checkpoint settings failed");
                sqlite3_busy_timeout(checkpoint_connection, 50);
            }
        }
    }
    void prepare(const std::string& text) {
        if (core) {
            if (!parsed.contains(text))
                parsed.emplace(text, sql::Statement(text));
        } else if (!prepared.contains(text)) {
            sqlite3_stmt* s = nullptr;
            const auto sql = durable && text == "BEGIN" ? std::string("BEGIN IMMEDIATE") : text;
            const int rc = sqlite3_prepare_v2(sqlite, sql.c_str(), -1, &s, nullptr);
            std::unique_ptr<sqlite3_stmt, Finalize> guard(s);
            check(rc == SQLITE_OK, sqlite3_errmsg(sqlite));
            prepared.emplace(text, std::move(guard));
        }
    }
    Rows exec(const std::string& text, const Row& parameters = {}) {
        prepare(text);
        if (core)
            return connection->execute(parsed.at(text), parameters).rows;
        auto* s = prepared.at(text).get();
        check(sqlite3_bind_parameter_count(s) == static_cast<int>(parameters.size()), "Parameter count");
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            int rc;
            if (auto n = std::get_if<std::int64_t>(&parameters[i]))
                rc = sqlite3_bind_int64(s, static_cast<int>(i + 1), *n);
            else {
                const auto& str = std::get<std::string>(parameters[i]);
                rc = sqlite3_bind_text(s, static_cast<int>(i + 1), str.data(), static_cast<int>(str.size()),
                                       SQLITE_TRANSIENT);
            }
            check(rc == SQLITE_OK, sqlite3_errmsg(sqlite));
        }
        Rows result;
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            auto row = current_row(s);
            result.push_back(std::move(row));
        }
        const std::string error = sqlite3_errmsg(sqlite);
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        check(rc == SQLITE_DONE, error);
        return result;
    }
    static Row current_row(sqlite3_stmt* s) {
        Row row;
        for (int i = 0; i < sqlite3_column_count(s); ++i) {
            if (sqlite3_column_type(s, i) == SQLITE_INTEGER)
                row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(s, i)));
            else if (sqlite3_column_type(s, i) == SQLITE_TEXT)
                row.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, i)),
                                             static_cast<std::size_t>(sqlite3_column_bytes(s, i))));
            else
                throw std::runtime_error("Unexpected result type");
        }
        return row;
    }
    template <class Visitor> void visit(const std::string& text, Visitor visitor) {
        prepare(text);
        if (core) {
            connection->query_each(parsed.at(text), [&](std::span<const Value> row) {
                visitor(row);
                return true;
            });
        } else {
            auto* statement = prepared.at(text).get();
            int rc;
            while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
                const auto row = current_row(statement);
                visitor(std::span<const Value>(row));
            }
            const std::string error = sqlite3_errmsg(sqlite);
            sqlite3_reset(statement);
            check(rc == SQLITE_DONE, error);
        }
    }
    void checkpoint() {
        if (core)
            db->checkpoint();
        else {
            int log = 0, done = 0;
            check(sqlite3_wal_checkpoint_v2(sqlite, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &log, &done) ==
                      SQLITE_OK,
                  sqlite3_errmsg(sqlite));
            check(log == done, "Incomplete SQLite checkpoint");
        }
    }
    void poll() {
        if (maintenance.valid() &&
            maintenance.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            maintenance.get();
            ++completed;
            last_checkpoint = std::chrono::steady_clock::now();
        }
        if (pending && !maintenance.valid()) {
            pending = false;
            ++started;
            if (core)
                maintenance = db->checkpoint_async();
            else
                maintenance = std::async(std::launch::async, [this] {
                    for (int attempt = 0; attempt < 1000; ++attempt) {
                        int log = 0, done = 0;
                        const int rc = sqlite3_wal_checkpoint_v2(checkpoint_connection, nullptr,
                                                                 SQLITE_CHECKPOINT_TRUNCATE, &log, &done);
                        if (rc == SQLITE_BUSY) {
                            ++busy_retries;
                            continue;
                        }
                        check(rc == SQLITE_OK && log == done, "SQLite background checkpoint failed");
                        return;
                    }
                    throw std::runtime_error("SQLite background checkpoint retry limit reached");
                });
        }
    }
    void start_measurement() {
        if (durable)
            compacted_bytes = std::filesystem::file_size(path);
        last_checkpoint = std::chrono::steady_clock::now();
    }
    void request_checkpoint() {
        if (adaptive) {
            std::error_code error;
            const auto bytes = std::filesystem::file_size(
                core ? path : std::filesystem::path(path.string() + "-wal"), error);
            if (error && error != std::errc::no_such_file_or_directory)
                throw std::filesystem::filesystem_error("Measure checkpoint backlog", path, error);
            const auto growth =
                error ? 0 : (core ? (bytes > compacted_bytes ? bytes - compacted_bytes : 0) : bytes);
            const auto threshold = std::max<std::uint64_t>(1024 * 1024, compacted_bytes / 2);
            const bool aged = std::chrono::steady_clock::now() - last_checkpoint >= std::chrono::seconds(1);
            if (growth < threshold && !(aged && growth > 0))
                return;
        }
        ++requested;
        if (!background) {
            checkpoint();
            ++started;
            ++completed;
            return;
        }
        if (pending)
            ++coalesced;
        pending = true;
        poll();
    }
    void drain() {
        while (maintenance.valid() || pending) {
            if (maintenance.valid()) {
                maintenance.get();
                ++completed;
            }
            poll();
        }
    }
    void integrity() {
        if (core)
            db->begin().integrity_check();
        else
            check(exec("PRAGMA integrity_check") == Rows{{std::string("ok")}}, "SQLite integrity");
    }
};
} // namespace application
