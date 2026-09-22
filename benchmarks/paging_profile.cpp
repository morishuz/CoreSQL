#include "coresql/core.hpp"
#include <chrono>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>
using namespace coresql;
namespace {
using Clock = std::chrono::steady_clock;
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "coresql-paging-XXXXXX").string();
        if (!::mkdtemp(pattern.data()))
            throw std::runtime_error("Cannot create benchmark directory");
        path = pattern;
    }
    ~Directory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double peak_mib() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage))
        throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return usage.ru_maxrss / (1024.0 * 1024.0);
#else
    return usage.ru_maxrss / 1024.0;
#endif
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto rows = argc > 1 ? std::stoull(argv[1]) : 20000;
        const auto cache_mib = argc > 2 ? std::stoull(argv[2]) : 1;
        const auto payload = argc > 3 ? std::stoull(argv[3]) : 4096;
        if (rows < 1000 || rows > 1000000 || cache_mib > 1024 || payload < 128 || payload > 65536)
            throw std::runtime_error(
                "Usage: paging_profile [rows 1000..1000000] [cache MiB 0..1024] [row bytes128..65536]");
        Directory temporary;
        const auto path = temporary.path / "data.core";
        const OpenOptions options{true, static_cast<std::size_t>(cache_mib * 1024 * 1024)};
        std::cout << "rows,payload_bytes,cache_mib,stage,elapsed_ms,peak_rss_mib,resident_bytes,pinned_bytes,"
                     "page_reads,page_writes,backing_bytes\n";
        auto report = [&](const char* stage, double elapsed, const Database& db) {
            const auto cache = db.cache_stats();
            std::cout << rows << ',' << payload << ',' << cache_mib << ',' << stage << ',' << elapsed << ','
                      << peak_mib() << ',' << cache.resident_bytes << ',' << cache.pinned_bytes << ','
                      << cache.page_reads << ',' << cache.page_writes << ',' << cache.backing_bytes << '\n';
        };
        {
            auto db = Database::open(path, {}, options);
            auto tx = db.begin();
            tx.create_table("items", {{"id", integer(), true}, {"body", text()}});
            tx.commit();
            auto start = Clock::now();
            for (std::size_t begin = 0; begin < rows; begin += 256) {
                auto batch = db.begin();
                for (auto i = begin; i < std::min<std::size_t>(begin + 256, rows); ++i)
                    batch.insert("items",
                                 {static_cast<std::int64_t>(i), std::string(payload, char('a' + i % 26))});
                batch.commit();
            }
            report("seed", milliseconds(start), db);
            start = Clock::now();
            db.checkpoint_async().get();
            report("checkpoint", milliseconds(start), db);
        }
        for (const auto* phase : {"first_reopen", "cached_reopen"}) {
            auto start = Clock::now();
            auto db = Database::open(path, {}, options);
            report(phase, milliseconds(start), db);
            start = Clock::now();
            auto cursor = db.cursor(Query{"items", {column("id"), column("body")}});
            std::uint64_t count = 0, sum = 0;
            while (auto row = cursor.next()) {
                const auto id = std::get<std::int64_t>((*row)[0]);
                const auto& body = std::get<std::string>((*row)[1]);
                if (body.size() != payload || body.front() != char('a' + id % 26))
                    throw std::runtime_error("Incorrect paged row");
                ++count;
                sum += id;
            }
            if (count != rows || sum != rows * (rows - 1) / 2)
                throw std::runtime_error("Incorrect scan");
            report("scan", milliseconds(start), db);
            start = Clock::now();
            for (std::size_t i = 0; i < 2000; ++i) {
                const auto id = static_cast<std::int64_t>((i * 7919) % rows);
                Query query{"items", {column("id")}, Predicate{column("id"), Compare::equal, literal(id)}};
                auto result = db.query(query);
                if (result.rows.size() != 1 || std::get<std::int64_t>(result.rows[0][0]) != id)
                    throw std::runtime_error("Incorrect indexed lookup");
            }
            report("lookup_2000", milliseconds(start), db);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
