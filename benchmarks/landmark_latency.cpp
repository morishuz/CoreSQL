#include "../examples/landmark_memory/worker.hpp"
#include <iostream>
#include <iomanip>
#include <unistd.h>
using namespace coresql;
namespace {
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        auto pattern = (std::filesystem::temp_directory_path() / "coresql-latency-XXXXXX").string();
        auto* result = mkdtemp(pattern.data());
        if (!result)
            throw Error(ErrorCode::io, "Cannot create latency directory");
        path = result;
    }
    ~Temporary() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        const auto rows = argc > 1 ? std::stoi(argv[1]) : 10000;
        const auto rounds = argc > 2 ? std::stoi(argv[2]) : 512;
        if (argc > 3 || rows < 1000 || rows > 100000 || rounds < 32 || rounds > 5000)
            throw Error(ErrorCode::state,
                        "Usage: coresql_landmark_latency [rows 1000..100000] [rounds 32..5000]");
        Temporary temp;
        const auto file = temp.path / "map";
        {
            landmarks::Worker create(file);
        }
        auto registry = landmarks::registry();
        {
            auto db = Database::open(file, registry);
            sql::Connection c(db, registry);
            const sql::Statement insert("INSERT INTO observations VALUES(?,?,?,?,?,?,?)");
            c.execute("BEGIN");
            for (int i = 0; i < rows; ++i)
                c.execute(insert, Row{std::int64_t{i}, std::string("v1"), std::string("map"), double(i % 64),
                                      double(i / 64), std::int64_t{0},
                                      vectors::value(landmarks::descriptor(i), 128)});
            c.execute("COMMIT");
            db.checkpoint();
        }
        std::cout << "phase,rows,kind,sequence,queue_ms,execute_ms,commit_ms,total_ms,success\n"
                  << std::setprecision(10);
        for (const std::string phase : {"unbatched_manual", "batched_manual", "batched_immediate",
                                        "batched_idle", "batched_background", "batched_paged_background"}) {
            landmarks::WorkerLimits limits{static_cast<std::size_t>(rows), 32};
            limits.batch_requests = phase == "unbatched_manual" ? 1 : 16;
            limits.checkpoint_every = phase.ends_with("manual") ? 0 : 256;
            limits.checkpoint_max_delay = std::chrono::milliseconds(phase == "batched_idle" ? 1000 : 0);
            limits.background_checkpoints = phase.ends_with("background");
            limits.reader_threads = phase.ends_with("background") ? 2 : 0;
            limits.page_cache_bytes = phase == "batched_paged_background" ? 16 * 1024 * 1024 : 0;
            landmarks::Worker worker(file, limits);
            std::uint64_t last = 0;
            auto collect = [&] {
                auto stats = worker.stats(last);
                for (const auto& sample : stats.samples) {
                    if (sample.sequence != last + 1)
                        throw Error(ErrorCode::state, "Latency sample buffer overran");
                    last = sample.sequence;
                    std::cout << phase << ',' << rows << ',' << sample.kind << ',' << sample.sequence << ','
                              << sample.queue_ms << ',' << sample.execute_ms << ',' << sample.commit_ms << ','
                              << sample.total_ms << ',' << sample.success << '\n';
                }
            };
            for (int round = 0; round < rounds; ++round) {
                std::vector<std::future<sql::Result>> writes, reads;
                const auto id = std::int64_t{round % rows};
                for (int i = 0; i < 8; ++i) {
                    landmarks::Observation observation{
                        id, round, "v1", "map", double(id % 64), double(id / 64), landmarks::descriptor(id)};
                    auto request = worker.try_ingest(std::array{observation});
                    if (!request)
                        throw Error(ErrorCode::state, "Latency write rejected");
                    writes.push_back(std::move(*request));
                }
                for (int i = 0; i < 16; ++i) {
                    landmarks::Search query{
                        "v1", "map", double(id % 64), double(id / 64), 0, landmarks::descriptor(id), {}};
                    auto request = worker.try_search(query);
                    if (!request)
                        throw Error(ErrorCode::state, "Latency read rejected");
                    reads.push_back(std::move(*request));
                }
                for (auto& write : writes)
                    if (write.get().changes != 1)
                        throw Error(ErrorCode::state, "Latency write failed");
                for (auto& read : reads)
                    landmarks::check(read.get().rows, {{id, 0.0}});
                collect();
            }
            auto checkpoint = worker.try_checkpoint();
            if (!checkpoint)
                throw Error(ErrorCode::state, "Checkpoint rejected");
            checkpoint->get();
            collect();
            auto stats = worker.stats(last);
            if (stats.accepted != stats.completed || stats.failures || stats.rejected)
                throw Error(ErrorCode::state, "Latency completion accounting differs");
            std::cerr << phase << ": verified " << rounds * 8 << " writes, " << rounds * 16 << " reads; "
                      << stats.committed_batches << " commits, " << stats.checkpoints << " checkpoints.\n";
        }
        auto db = Database::open(file, registry);
        db.begin().integrity_check();
        sql::Connection c(db, registry);
        auto actual = c.execute("SELECT id,seen FROM observations ORDER BY id").rows;
        if (actual.size() != static_cast<std::size_t>(rows))
            throw Error(ErrorCode::state, "Latency row count differs");
        for (int id = 0; id < rows; ++id) {
            const auto expected = id < rounds ? id + (rounds - 1 - id) / rows * rows : 0;
            landmarks::check({actual[static_cast<std::size_t>(id)]},
                             {{std::int64_t{id}, std::int64_t{expected}}});
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
