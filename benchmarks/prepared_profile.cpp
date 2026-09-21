#include "coresql/sql.hpp"
#include <chrono>
#include <iostream>
using namespace coresql;
int main() {
    try {
        Registry registry;
        sql::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute("CREATE TABLE t(id INTEGER PRIMARY KEY,n INTEGER)");
        c.execute("INSERT INTO t VALUES(1,11),(2,22)");
        const sql::Statement query("SELECT n FROM t WHERE id=?");
        constexpr int iterations = 10000;
        std::cout << "pattern,cache,trial,iterations,milliseconds,hits,misses\n";
        for (bool varying : {false, true})
            for (int trial = 0; trial < 7; ++trial)
                for (int turn = 0; turn < 2; ++turn) {
                    const bool enabled = (trial + turn) % 2 == 0;
                    c.enable_query_cache(enabled);
                    c.execute(query, Row{std::int64_t{1}});
                    auto before = c.query_cache_stats();
                    std::int64_t sum = 0;
                    auto start = std::chrono::steady_clock::now();
                    for (int i = 0; i < iterations; ++i) {
                        auto result = c.execute(query, Row{std::int64_t{varying ? i % 2 + 1 : 1}});
                        sum += std::get<std::int64_t>(result.rows[0][0]);
                    }
                    auto ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                            .count();
                    const auto expected = std::int64_t{iterations} * (varying ? 33 : 22) / 2;
                    if (sum != expected)
                        throw Error(ErrorCode::state, "Prepared result checksum differs");
                    const auto after = c.query_cache_stats();
                    std::cout << (varying ? "alternating" : "constant") << ',' << enabled << ',' << trial
                              << ',' << iterations << ',' << ms << ',' << after.hits - before.hits << ','
                              << after.misses - before.misses << '\n';
                }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
