#include "allocation_profile.hpp"
#include "coresql/core.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>

using namespace coresql;

// A deterministic native-API workload. Setup and exact answer checks are untimed.
int main(int argc, char** argv) {
    try {
        if (argc != 7)
            throw std::runtime_error(
                "Usage: join_profile general|equality|scan rows fanout text_bytes repeats time|alloc");
        const std::string mode = argv[1], profile = argv[6];
        const auto n = std::stoull(argv[2]), fanout = std::stoull(argv[3]);
        const auto width = std::stoull(argv[4]), repeats = std::stoull(argv[5]);
        if (!n || n > 100000 || !fanout || fanout > 32 || n % fanout || width > 4096 || !repeats ||
            repeats > 100 || (mode != "general" && mode != "equality" && mode != "scan") ||
            (profile != "time" && profile != "alloc"))
            throw std::runtime_error("Invalid workload");
        const auto keys = n / fanout;
        Registry registry;
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("l", {{"k", integer()}, {"payload", text()}});
        tx.create_table("r", {{"k", integer()}, {"v", integer()}});
        const std::string payload(width, 'x');
        for (std::uint64_t i = 0; i < n; ++i) {
            tx.insert("l", {std::int64_t(i % keys), payload});
            tx.insert("r", {std::int64_t(i % keys), std::int64_t(i)});
        }
        tx.commit();
        Query query{"l", {column("a", "payload")}};
        query.alias = "a";
        query.repeatable = true;
        if (mode != "scan") {
            query.select.push_back(column("b", "v"));
            query.join = Join{"r", "b", column("a", "k"), column("b", "k")};
            if (mode == "general")
                query.join->on =
                    all_of({Predicate{column("a", "k"), Compare::equal, column("b", "k")},
                            Predicate{column("b", "v"), Compare::greater_equal, literal(std::int64_t{0})}});
        }
        auto verify = [&](const Result& result) {
            if (result.rows.size() != n * (mode == "scan" ? 1 : fanout))
                throw std::runtime_error("Wrong row count");
            std::size_t row = 0;
            for (std::uint64_t i = 0; i < n; ++i)
                for (std::uint64_t j = 0; j < (mode == "scan" ? 1 : fanout); ++j) {
                    const auto& values = result.rows[row++];
                    if (std::get<std::string>(values[0]) != payload ||
                        (mode != "scan" &&
                         std::get<std::int64_t>(values[1]) != std::int64_t(i % keys + j * keys)))
                        throw std::runtime_error("Wrong row contents or order");
                }
        };
        verify(db.query(query));
        std::vector<double> times;
        for (std::uint64_t i = 0; i < repeats; ++i) {
            if (profile == "alloc")
                allocation_profile::begin();
            auto start = std::chrono::steady_clock::now();
            auto result = db.query(query);
            auto end = std::chrono::steady_clock::now();
            if (profile == "alloc")
                allocation_profile::end();
            times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            verify(result);
        }
        std::sort(times.begin(), times.end());
        std::cout << mode << ',' << n << ',' << fanout << ',' << width << ',' << repeats << ','
                  << times[times.size() / 2] << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
