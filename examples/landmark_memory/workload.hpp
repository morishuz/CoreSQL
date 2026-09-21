#pragma once
#include "coresql/sql.hpp"
#include "coresql/vector.hpp"
#include <array>
#include <cmath>

// A deterministic application fixture, not a perception or navigation model.
namespace landmarks {
using namespace coresql;
inline constexpr std::size_t dimensions = 128;
inline Registry registry() {
    Registry r;
    sql::install(r);
    return r;
}
inline std::array<float, dimensions> descriptor(std::int64_t id) {
    std::array<float, dimensions> values{};
    auto state = static_cast<std::uint32_t>(id) + 1;
    for (auto& value : values) {
        state = state * 1664525U + 1013904223U;
        value = static_cast<float>(state >> 16) / 65536.0F;
    }
    return values;
}
inline void create(sql::Connection& c) {
    c.execute("CREATE TABLE landmarks(id INTEGER PRIMARY KEY, model TEXT NOT NULL, "
              "x REAL NOT NULL, y REAL NOT NULL, seen INTEGER NOT NULL, descriptor VECTOR(128) NOT NULL)");
    c.execute("CREATE INDEX landmarks_local ON landmarks(model,y,x)");
}
inline void insert(sql::Connection& c, std::int64_t first, std::int64_t count) {
    sql::Statement statement("INSERT INTO landmarks VALUES(?,?,?,?,?,?)");
    for (std::int64_t id = first; id < first + count; ++id) {
        c.execute(statement,
                  std::array<Value, 6>{id, std::string(id % 3 ? "model-a" : "model-b"),
                                       static_cast<double>(id % 100), static_cast<double>(id / 100),
                                       std::int64_t{0}, vectors::value(descriptor(id), dimensions)});
    }
}
inline sql::Statement search_statement() {
    return sql::Statement("SELECT id,vector_squared_l2(descriptor,?1) AS distance FROM landmarks "
                          "WHERE model=?2 AND x BETWEEN ?3 AND ?4 AND y BETWEEN ?5 AND ?6 "
                          "ORDER BY distance,id LIMIT ?7");
}
inline std::array<Value, 7> search_parameters(std::int64_t id) {
    const auto x = static_cast<double>(id % 100), y = static_cast<double>(id / 100);
    return {vectors::value(descriptor(id), dimensions),
            std::string("model-a"),
            x - 10,
            x + 10,
            y - 10,
            y + 10,
            std::int64_t{5}};
}
inline std::vector<Row> oracle(std::int64_t rows, std::int64_t id) {
    const auto query = descriptor(id);
    std::vector<Row> result;
    for (std::int64_t candidate = 0; candidate < rows; ++candidate) {
        if (candidate % 3 == 0 || std::abs(candidate % 100 - id % 100) > 10 ||
            std::abs(candidate / 100 - id / 100) > 10)
            continue;
        const auto value = descriptor(candidate);
        double distance = 0;
        for (std::size_t i = 0; i < dimensions; ++i) {
            const auto d = static_cast<double>(value[i]) - query[i];
            distance += d * d;
        }
        result.push_back({candidate, distance});
    }
    std::sort(result.begin(), result.end(), [](const Row& a, const Row& b) {
        return std::pair{std::get<double>(a[1]), std::get<std::int64_t>(a[0])} <
               std::pair{std::get<double>(b[1]), std::get<std::int64_t>(b[0])};
    });
    if (result.size() > 5)
        result.resize(5);
    return result;
}
inline void check(const std::vector<Row>& actual, const std::vector<Row>& expected) {
    if (actual != expected)
        throw Error(ErrorCode::state, "Landmark result differs from independent oracle");
}
} // namespace landmarks
