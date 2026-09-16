#include "coresql/core.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <sys/resource.h>
using namespace coresql;

// Same public-API harness can be compiled against historical engine revisions.
int main(int argc, char** argv) {
    try {
        const std::size_t n = argc > 1 ? std::stoull(argv[1]) : 100000;
        const std::size_t repeats = argc > 2 ? std::stoull(argv[2]) : 9;
        if (n < 10 || n > 1000000 || !repeats) throw std::runtime_error("Invalid workload size");
        auto shuffled = [](std::size_t i) { return static_cast<std::int64_t>((i * 7919) % 1000); };
        Database db;
        auto tx = db.begin();
        tx.create_table("t", {{"id", integer()}, {"shuffled", integer()}, {"score", real()}, {"name", text()}});
        for (std::size_t i = 0; i < n; ++i)
            tx.insert("t", {static_cast<std::int64_t>(i), shuffled(i), double(i % 100), "row" + std::to_string(i)});
        tx.commit();
        std::cout << "workload,rows,repeats,median_ms,min_ms,max_ms,process_peak_rss_bytes\n";
        for (const auto& name : {"id", "shuffled", "score", "name"}) {
            const std::string mode = name;
            std::vector<std::size_t> expected(n);
            std::iota(expected.begin(), expected.end(), 0);
            std::stable_sort(expected.begin(), expected.end(), [&](auto a, auto b) {
                if (mode == "id") return a > b;
                if (mode == "shuffled") return shuffled(a) > shuffled(b);
                if (mode == "score") return a % 100 > b % 100;
                return "row" + std::to_string(a) > "row" + std::to_string(b);
            });
            Query query{"t", {column("id")}, {}, {Order{column(mode), true}}};
            auto run = [&] {
                auto result = db.query(query);
                if (result.rows.size() != n) throw std::runtime_error("Wrong result count");
                for (std::size_t i = 0; i < n; ++i)
                    if (std::get<std::int64_t>(result.rows[i][0]) != static_cast<std::int64_t>(expected[i]))
                        throw std::runtime_error("Wrong order or unstable tie");
            };
            run();
            std::vector<double> times;
            for (std::size_t i = 0; i < repeats; ++i) {
                const auto start = std::chrono::steady_clock::now(); run();
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times.begin(), times.end());
            rusage usage{};
            if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
            const auto peak = usage.ru_maxrss;
#else
            const auto peak = usage.ru_maxrss * 1024;
#endif
            std::cout << mode << ',' << n << ',' << repeats << ',' << times[times.size()/2]
                      << ',' << times.front() << ',' << times.back() << ',' << peak << '\n';
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
