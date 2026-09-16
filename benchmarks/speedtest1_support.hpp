#pragma once
// Faithful port of selected testset_main workloads, not a full speedtest1 score.
// Input generators are linked directly from the unchanged upstream test source.
#include "coresql/core.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <string_view>

extern "C" {
unsigned swizzle(unsigned, unsigned);
unsigned roundup_allones(unsigned);
int speedtest1_numbername(unsigned, char*, int);
}
using namespace coresql;
using Rows = std::vector<Row>;
using Clock = std::chrono::steady_clock;


void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
struct SQLite {
    sqlite3* db = nullptr;
    SQLite() {
        int rc = sqlite3_open(":memory:", &db);
        if (rc != SQLITE_OK) {
            std::string message = db ? sqlite3_errmsg(db) : "SQLite allocation failure";
            sqlite3_close(db); db = nullptr; throw std::runtime_error(message);
        }
    }
    ~SQLite() { sqlite3_close(db); }
    SQLite(const SQLite&) = delete;
    void exec(const std::string& sql) {
        require(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
                sqlite3_errmsg(db));
    }
};
struct Statement {
    sqlite3_stmt* stmt = nullptr;
    Statement(SQLite& db, const std::string& sql) {
        require(sqlite3_prepare_v2(db.db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK,
                sqlite3_errmsg(db.db));
    }
    ~Statement() { sqlite3_finalize(stmt); }
    Statement(const Statement&) = delete;
    void check(int rc) { require(rc == SQLITE_OK, sqlite3_errmsg(sqlite3_db_handle(stmt))); }
    int step() {
        int rc = sqlite3_step(stmt);
        require(rc == SQLITE_ROW || rc == SQLITE_DONE, sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return rc;
    }
    void bind(int i, const Value& v) {
        if (auto n = std::get_if<std::int64_t>(&v)) check(sqlite3_bind_int64(stmt, i, *n));
        else {
            const auto& s = std::get<std::string>(v);
            check(sqlite3_bind_text(stmt, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT));
        }
    }
    void reset() { check(sqlite3_reset(stmt)); }
    Rows rows() {
        Rows result;
        while (step() == SQLITE_ROW) {
            Row row;
            for(int i=0;i<sqlite3_column_count(stmt);++i) switch(sqlite3_column_type(stmt,i)) {
            case SQLITE_INTEGER: row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(stmt,i))); break;
            case SQLITE_FLOAT: row.emplace_back(sqlite3_column_double(stmt,i)); break;
            case SQLITE_NULL: row.emplace_back(Null(integer())); break;
            case SQLITE_TEXT: row.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt,i)),static_cast<std::size_t>(sqlite3_column_bytes(stmt,i)))); break;
            default: throw std::runtime_error("Unsupported benchmark result");
            }
            result.push_back(std::move(row));
        }
        reset(); return result;
    }
};
std::string number(unsigned n) {
    char buffer[2000];
    int length = speedtest1_numbername(n, buffer, sizeof(buffer));
    return {buffer, static_cast<std::size_t>(length)};
}
Row input(unsigned i, unsigned mask, bool ordered) {
    const auto x = swizzle(i, mask);
    return {static_cast<std::int64_t>(ordered ? i : x),
            static_cast<std::int64_t>(ordered ? x : i), number(x)};
}
std::string pattern(unsigned i) {
    // Preserve upstream's exact buffer offsets: the final name character is
    // overwritten by '%'. Do not "correct" this to '%' + number(i) + '%'.
    auto name = number(i);
    return "%" + name.substr(0, name.size() - 1) + "%";
}
Registry registry() {
    Registry r;
    r.add(Function{"benchmark.ascii_contains", [](std::span<const Type> args) {
        require(args.size() == 2 && args[0] == text() && args[1] == text(), "contains types");
        return integer();
    }, [](std::span<const Value> args) -> Value {
        return std::int64_t{std::get<std::string>(args[0]).find(std::get<std::string>(args[1]))
                           != std::string::npos};
    }});
    return r;
}
void equal(const Rows& actual, const Rows& expected) {
    require(actual.size() == expected.size(), "row count mismatch");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(actual[i].size() == 3, "column count mismatch");
        require(std::get<std::int64_t>(actual[i][0]) == std::get<std::int64_t>(expected[i][0]) &&
                std::get<std::int64_t>(actual[i][1]) == std::get<std::int64_t>(expected[i][1]) &&
                std::get<std::string>(actual[i][2]) == std::get<std::string>(expected[i][2]),
                "result value/order mismatch at row " + std::to_string(i));
    }
}
