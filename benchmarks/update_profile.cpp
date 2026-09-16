#include "coresql/core.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
using namespace coresql;
int main() {
    Database db; auto tx = db.begin();
    tx.create_table("items", {{"id", integer()}, {"score", real()}, {"name", text()}});
    for (std::int64_t i = 0; i < 100000; ++i) tx.insert("items", {i, double(i % 100), "row" + std::to_string(i)});
    tx.commit();
    std::vector<double> samples;
    for (int trial = -1; trial < 9; ++trial) {
        auto start = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < 50; ++repeat) {
            auto edit = db.begin();
            auto count = edit.update("items", {{"name", literal(std::string("changed"))}},
                Predicate{column("id"), Compare::equal, literal(std::int64_t{50000})});
            if (count != 1) return 1;
            edit.rollback();
        }
        if (trial >= 0) samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 50);
    }
    auto result = db.query({"items", {column("name")}, Predicate{column("id"), Compare::equal, literal(std::int64_t{50000})}});
    if (result.rows.size() != 1 || std::get<std::string>(result.rows[0][0]) != "row50000") return 1;
    std::sort(samples.begin(), samples.end());
    std::cout << "rows,median_us,min_us,max_us\n100000," << samples[4] << ',' << samples.front() << ',' << samples.back() << '\n';
}
