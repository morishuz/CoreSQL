#include "../examples/landmark_memory/worker.hpp"
#include <chrono>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>
namespace {
std::uint64_t peak() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage))
        throw coresql::Error(coresql::ErrorCode::state, "getrusage failed");
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        auto pattern = (std::filesystem::temp_directory_path() / "coresql-soak-XXXXXX").string();
        auto* created = mkdtemp(pattern.data());
        if (!created)
            throw coresql::Error(coresql::ErrorCode::io, "Cannot create soak directory");
        path = created;
    }
    ~Temporary() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
} // namespace
int main(int argc, char** argv) {
    using namespace coresql;
    try {
        const auto cycles = argc >= 2 ? std::stoi(argv[1]) : 3000;
        const auto requested_rows = argc == 3 ? std::stoi(argv[2]) : 1000;
        if (argc > 3 || cycles < 100 || cycles > 1000000 || requested_rows < 1000 || requested_rows > 100000)
            throw Error(ErrorCode::state,
                        "Usage: coresql_landmark_soak [cycles 100..1000000] [rows 1000..100000]");
        Temporary temp;
        std::cout << "cycle,elapsed_ms,process_peak_rss_bytes,file_bytes\n";
        const auto start = std::chrono::steady_clock::now();
        const auto file = temp.path / "map";
        const auto rows = static_cast<std::size_t>(requested_rows);
        {
            landmarks::Worker seed(file, {rows, 8});
            for (std::size_t first = 0; first < rows; first += 32) {
                std::vector<landmarks::Observation> batch;
                for (std::size_t id = first; id < std::min(first + 32, rows); ++id)
                    batch.push_back({static_cast<std::int64_t>(id), 0, "descriptor-v1", "map-1",
                                     static_cast<double>(id % 64), static_cast<double>(id / 64),
                                     landmarks::descriptor(static_cast<std::int64_t>(id))});
                auto accepted = seed.try_ingest(batch);
                if (!accepted || accepted->get().changes != batch.size())
                    throw Error(ErrorCode::state, "Soak seed failed");
            }
        }
        std::size_t backpressure = 0;

        for (int phase = 0; phase < 10; ++phase) {
            landmarks::Worker worker(file, {rows, 8});
            for (int i = phase * cycles / 10; i < (phase + 1) * cycles / 10; ++i) {
                const auto id = static_cast<std::int64_t>(i % rows);
                landmarks::Observation item{id,
                                            i,
                                            "descriptor-v1",
                                            "map-1",
                                            static_cast<double>(id % 64),
                                            static_cast<double>(id / 64),
                                            landmarks::descriptor(id)};
                if (i % 25 == 0) {
                    auto erased = worker.try_erase(std::array{id});
                    if (!erased || erased->get().changes != 1)
                        throw Error(ErrorCode::state, "Soak erase failed");
                }
                auto accepted = worker.try_ingest(std::array{item});
                if (!accepted || accepted->get().changes != 1)
                    throw Error(ErrorCode::state, "Soak write was not acknowledged");
                landmarks::Search search{"descriptor-v1", "map-1", item.x, item.y, 0, item.descriptor, {}};
                auto answer = worker.try_search(search);
                if (!answer)
                    throw Error(ErrorCode::state, "Soak read rejected");
                landmarks::check(answer->get().rows, {{id, 0.0}});
                if (i % 300 == 0) {
                    std::vector<std::future<sql::Result>> burst;
                    for (int j = 0; j < 64; ++j) {
                        auto request = worker.try_search(search);
                        if (request)
                            burst.push_back(std::move(*request));
                        else
                            ++backpressure;
                    }
                    for (auto& request : burst)
                        landmarks::check(request.get().rows, {{id, 0.0}});
                }
                if (i % 100 == 0 || i + 1 == cycles)
                    std::cout << i + 1 << ','
                              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                           start)
                                     .count()
                              << ',' << peak() << ',' << std::filesystem::file_size(file) << '\n';
            }
        }
        auto registry = landmarks::registry();
        auto db = Database::open(file, registry);
        sql::Connection c(db, registry);
        auto data = c.execute("SELECT id,seen FROM observations ORDER BY id").rows;
        if (data.size() != rows)
            throw Error(ErrorCode::state, "Soak row count changed");
        for (std::size_t id = 0; id < rows; ++id) {
            const auto last = id < static_cast<std::size_t>(cycles)
                                  ? id + (static_cast<std::size_t>(cycles) - 1 - id) / rows * rows
                                  : 0;
            landmarks::check({data[id]}, {{static_cast<std::int64_t>(id), static_cast<std::int64_t>(last)}});
        }
        db.begin().integrity_check();
        std::cerr << "Verified " << cycles
                  << " durable ingest cycles, pruning, exact searches and ten worker reopenings; "
                  << backpressure << " requests rejected with backpressure.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
