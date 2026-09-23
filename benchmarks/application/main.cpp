#include "backend.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <unistd.h>

namespace {
using namespace application;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
double elapsed(Time a, Time b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
struct Temporary {
    std::filesystem::path path;
    explicit Temporary(const std::filesystem::path& root) {
        auto name = (root / "application-XXXXXX").string();
        auto* p = mkdtemp(name.data());
        check(p, "Cannot create temporary directory");
        path = p;
    }
    ~Temporary() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
struct Workload {
    Backend& engine;
    std::int64_t n, operations, payload;
    std::string only;
    bool covering;
    std::vector<std::int64_t> values;
    std::int64_t next, expired = 0;
    const std::string point = "SELECT v,body FROM events WHERE id=?";
    const std::string update = "UPDATE events SET v=? WHERE id=?";
    const std::string insert = "INSERT INTO events VALUES(?,?,?,?,?)";
    const std::string upsert =
        "INSERT INTO events VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET v=excluded.v";
    const std::string page = "SELECT id,ts FROM events WHERE device=? AND ts>? ORDER BY ts LIMIT 20";
    const std::string recent = "SELECT id,ts FROM events WHERE device=? ORDER BY ts DESC LIMIT 20";
    const std::string range = "SELECT count(*),sum(v) FROM events WHERE ts>=? AND ts<?";
    const std::string join = "SELECT e.id,d.label FROM events AS e JOIN devices AS d ON e.device=d.id WHERE "
                             "e.id>=? AND e.id<? ORDER BY e.id";
    Workload(Backend& e, std::int64_t rows, std::int64_t ops, std::int64_t bytes, std::string phase,
             bool covered)
        : engine(e), n(rows), operations(ops), payload(bytes), only(std::move(phase)), covering(covered),
          next(rows) {
        values.reserve(static_cast<std::size_t>(rows + ops * 17));
        for (std::int64_t i = 0; i < n; ++i)
            values.push_back(i % 97);
    }
    std::string body(std::int64_t id) const {
        return std::string(static_cast<std::size_t>(payload), char('a' + id % 26));
    }
    Row row(std::int64_t id, std::int64_t v) const { return {id, id % 100, id, v, body(id)}; }
    void emit(const std::string& phase, std::int64_t i, double ms, std::int64_t units = 1) {
        if (std::getenv("CORESQL_APPLICATION_PROFILE"))
            return;
        const auto [rss, peak] = memory();
        std::uint64_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(engine.path.parent_path()))
            if (entry.is_regular_file())
                files += entry.file_size();
        CacheStats cache{};
        StorageStats storage{};
        if (engine.core) {
            cache = engine.db->cache_stats();
            storage = engine.db->storage_stats();
        }
        std::cout << phase << ',' << i << ',' << std::setprecision(12) << ms << ',' << units << ',' << rss
                  << ',' << peak << ',' << files << ',' << cache.page_reads << ',' << cache.page_writes << ','
                  << storage.bytes_written << ',' << storage.checkpoints << '\n';
    }
    bool enabled(const std::string& phase) const {
        if (only != "all" && only != phase)
            return false;
        std::cerr << "PHASE_READY " << phase << std::endl;
        return true;
    }
    template <class F>
    void transaction(const std::string& phase, std::int64_t i, std::int64_t units, F action,
                     bool maintain = false) {
        const auto start = Clock::now();
        engine.exec("BEGIN");
        action();
        const auto executed = Clock::now();
        engine.exec("COMMIT");
        const auto committed = Clock::now();
        if (maintain)
            engine.checkpoint();
        const auto end = Clock::now();
        emit(phase + "_execute", i, elapsed(start, executed), units);
        emit(phase + "_commit", i, elapsed(executed, committed));
        if (maintain)
            emit("checkpoint", i, elapsed(committed, end));
        emit(phase, i, elapsed(start, end), units);
    }
    void seed() {
        const auto start = Clock::now();
        engine.exec("CREATE TABLE devices(id INTEGER PRIMARY KEY,label TEXT NOT NULL)");
        engine.exec("CREATE TABLE events(id INTEGER PRIMARY KEY,device INTEGER NOT NULL,ts INTEGER NOT "
                    "NULL,v INTEGER NOT NULL,body TEXT NOT NULL)");
        engine.exec("BEGIN");
        for (std::int64_t i = 0; i < 100; ++i)
            engine.exec("INSERT INTO devices VALUES(?,?)", {i, std::string("device-") + std::to_string(i)});
        engine.exec("COMMIT");
        for (std::int64_t base = 0; base < n; base += 256) {
            engine.exec("BEGIN");
            for (auto i = base; i < std::min(n, base + 256); ++i)
                engine.exec(insert, row(i, values[i]));
            engine.exec("COMMIT");
        }
        engine.exec(covering ? "CREATE INDEX events_device_time ON events(device,ts,id)"
                             : "CREATE INDEX events_device_time ON events(device,ts)");
        engine.exec("CREATE INDEX events_time ON events(ts)");
        if (engine.durable) {
            engine.checkpoint();
            engine.close();
            engine.open();
        }
        emit("load_and_initial_checkpoint", 0, elapsed(start, Clock::now()), n);
        for (const auto& text :
             {point, update, insert, upsert, page, recent, range, join, std::string("BEGIN"),
              std::string("COMMIT"), std::string("DELETE FROM events WHERE ts<?")})
            engine.prepare(text);
    }
    void reads() {
        for (const std::string phase :
             {"point_read", "event_page", "latest_events", "time_range", "small_join"}) {
            if (!enabled(phase))
                continue;
            for (std::int64_t i = -1; i < operations; ++i) {
                const auto key = ((i + 1) * 7919) % (n - 100);
                Rows expected;
                Row args;
                std::string sql;
                if (phase == "point_read") {
                    sql = point;
                    args = {key};
                    expected = {{values[key], body(key)}};
                } else if (phase == "event_page" || phase == "latest_events") {
                    const auto device = key % 100;
                    if (phase == "event_page") {
                        sql = page;
                        args = {device, key};
                        for (auto id = key + 100; id < n && expected.size() < 20; id += 100)
                            expected.push_back({id, id});
                    } else {
                        sql = recent;
                        args = {device};
                        for (auto id = n - 1 - (n - 1 - device) % 100; id >= 0 && expected.size() < 20;
                             id -= 100)
                            expected.push_back({id, id});
                    }
                } else if (phase == "time_range") {
                    sql = range;
                    args = {key, key + 100};
                    std::int64_t sum = 0;
                    for (auto id = key; id < key + 100; ++id)
                        sum += values[id];
                    expected = {{std::int64_t{100}, sum}};
                } else {
                    sql = join;
                    args = {key, key + 10};
                    for (auto id = key; id < key + 10; ++id)
                        expected.push_back({id, std::string("device-") + std::to_string(id % 100)});
                }
                const auto start = Clock::now();
                const auto result = engine.exec(sql, args);
                const auto end = Clock::now();
                if (i >= 0)
                    emit(phase, i, elapsed(start, end));
                check(result == expected, phase + " answer mismatch");
            }
        }
    }
    void writes() {
        for (const std::string phase : {"point_update", "upsert_existing", "upsert_unchanged", "upsert_new",
                                        "append_16", "expire_16", "mixed"}) {
            if (!enabled(phase))
                continue;
            for (std::int64_t i = 0; i < operations; ++i) {
                const auto key = n / 2 + (i * 7919) % (n / 2);
                const auto value = phase == "upsert_unchanged"
                                       ? values[key]
                                       : (phase == "upsert_existing" ? 10000000 : 1000) + i;
                if (phase == "point_update" || phase == "upsert_existing" || phase == "upsert_unchanged") {
                    transaction(phase, i, 1, [&] {
                        if (phase == "point_update")
                            engine.exec(update, {value, key});
                        else
                            engine.exec(upsert, row(key, value));
                    });
                    values[key] = value;
                } else if (phase == "upsert_new" || phase == "append_16") {
                    const auto count = phase == "upsert_new" ? 1 : 16;
                    transaction(phase, i, count, [&] {
                        for (auto id = next; id < next + count; ++id)
                            engine.exec(phase == "upsert_new" ? upsert : insert, row(id, value));
                    });
                    next += count;
                    values.insert(values.end(), static_cast<std::size_t>(count), value);
                } else if (phase == "expire_16") {
                    // Retain the upper half for the subsequent state workload.
                    if (expired + 16 > n / 2)
                        break;
                    transaction(phase, i, 16,
                                [&] { engine.exec("DELETE FROM events WHERE ts<?", {expired + 16}); });
                    expired += 16;
                } else {
                    std::vector<Rows> actual;
                    actual.reserve(8);
                    const auto second = n / 2 + (key + 31) % (n / 2);
                    transaction(
                        phase, i, 10,
                        [&] {
                            for (int j = 0; j < 8; ++j)
                                actual.push_back(engine.exec(point, {n / 2 + (key + j * 31) % (n / 2)}));
                            engine.exec(update, {value, key});
                            engine.exec(update, {value + 1, second});
                        },
                        engine.durable && i % 50 == 49);
                    for (int j = 0; j < 8; ++j) {
                        const auto read_key = n / 2 + (key + j * 31) % (n / 2);
                        check(actual[j] == Rows{{values[read_key], body(read_key)}}, "Mixed read mismatch");
                    }
                    values[key] = value;
                    values[second] = value + 1;
                }
            }
            verify();
        }
    }
    void verify() {
        // Stream once: verification must not depend on primary-key range planning.
        std::vector<bool> seen(static_cast<std::size_t>(next - expired));
        std::size_t count = 0;
        engine.visit("SELECT id,device,ts,v,body FROM events", [&](std::span<const Value> actual) {
            check(actual.size() == 5, "Final column count mismatch");
            const auto id = std::get<std::int64_t>(actual[0]);
            check(id >= expired && id < next, "Unexpected row identity");
            const auto position = static_cast<std::size_t>(id - expired);
            check(!seen[position], "Duplicate row identity");
            seen[position] = true;
            const auto expected = row(id, values[id]);
            check(std::equal(actual.begin(), actual.end(), expected.begin()), "Final content mismatch");
            ++count;
        });
        check(count == seen.size(), "Final row count mismatch");
    }
    void run() {
        seed();
        reads();
        writes();
        verify();
        engine.integrity();
        if (engine.durable && only == "all") {
            for (int i = 0; i < 3; ++i) {
                engine.close();
                const auto start = Clock::now();
                engine.open();
                emit("reopen", i, elapsed(start, Clock::now()));
                verify();
            }
        }
        std::cerr << "VERIFIED all requested operations, final contents and integrity\n";
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 9 || argc == 10,
              "Usage: application ENGINE memory|durable ROWS PAYLOAD_BYTES OPERATIONS CACHE_MIB "
              "SCRATCH_DIR PHASE|all [covering]");
        const bool covering = argc == 10;
        check(!covering || std::string(argv[9]) == "covering", "Unknown index variant");
        const std::string engine = argv[1], mode = argv[2];
        const auto rows = std::stoll(argv[3]), payload = std::stoll(argv[4]), ops = std::stoll(argv[5]),
                   cache = std::stoll(argv[6]);
        check((engine == "coresql" || engine == "sqlite") && (mode == "memory" || mode == "durable"),
              "Invalid engine/mode");
        check(rows >= 1000 && rows <= 1000000 && payload >= 0 && payload <= 4096 && ops >= 1 &&
                  ops <= 1000000 && cache >= 0 && cache <= 1024,
              "Arguments out of bounds");
        const std::string phase = argv[8];
        check(phase == "all" || phase == "point_read" || phase == "event_page" || phase == "latest_events" ||
                  phase == "time_range" || phase == "small_join" || phase == "point_update" ||
                  phase == "upsert_existing" || phase == "upsert_unchanged" || phase == "upsert_new" ||
                  phase == "append_16" || phase == "expire_16" || phase == "mixed",
              "Unknown phase");
        Temporary temp(argv[7]);
        Backend backend(engine == "coresql", mode == "durable", static_cast<std::size_t>(cache) * 1024 * 1024,
                        temp.path / "db");
        std::cerr
            << "SQLite " << sqlite3_sourceid() << "; compiler=" << __VERSION__
            << "; SQL template cache disabled; prepared SQL; SQLite WAL/FULL/fullfsync/checkpoint_fullfsync; "
               "synchronous checkpoints every 50 mixed transactions\n";
        std::cout << "phase,sample,milliseconds,units,rss_bytes,peak_rss_bytes,file_bytes,core_page_reads,"
                     "core_page_writes,core_written_bytes,core_checkpoints\n";
        std::cerr << "Event index: " << (covering ? "(device,ts,id)" : "(device,ts)") << '\n';
        Workload(backend, rows, ops, payload, phase, covering).run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
