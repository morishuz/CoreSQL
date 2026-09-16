#include "coresql/json.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
using namespace coresql;
namespace {
void check(bool ok) { if (!ok) throw std::runtime_error("JSON benchmark result mismatch"); }
struct SQL {
    sqlite3* db = nullptr;
    SQL() { check(sqlite3_open(":memory:", &db) == SQLITE_OK); }
    ~SQL() { sqlite3_close(db); }
    void exec(const std::string& text) {
        if (sqlite3_exec(db, text.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
};
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(SQL& sql, const std::string& text) { check(sqlite3_prepare_v2(sql.db, text.c_str(), -1, &p, nullptr) == SQLITE_OK); }
    ~Statement() { sqlite3_finalize(p); }
    void bind(std::int64_t id, const std::string& doc) {
        check(sqlite3_bind_int64(p, 1, id) == SQLITE_OK);
        check(sqlite3_bind_text(p, 2, doc.data(), static_cast<int>(doc.size()), SQLITE_TRANSIENT) == SQLITE_OK);
    }
    void insert() { check(sqlite3_step(p) == SQLITE_DONE); check(sqlite3_reset(p) == SQLITE_OK); }
};
template<class F> double time(int repeats, F&& work) {
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repeats; ++i) work();
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / repeats;
}
template<class C, class S> void compare(const std::string& label, std::size_t rows, int repeats, C&& core, S&& sql) {
    core(); sql(); std::vector<double> a, b;
    for (int i = 0; i < 7; ++i) {
        if (i % 2) { b.push_back(time(repeats, sql)); a.push_back(time(repeats, core)); }
        else { a.push_back(time(repeats, core)); b.push_back(time(repeats, sql)); }
    }
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    std::cout << label << ',' << rows << ',' << a[3] << ',' << b[3] << '\n';
}
}
int main(int argc, char** argv) {
    try {
        std::size_t n = argc > 1 ? std::stoull(argv[1]) : 10000; check(n >= 10 && n <= 1000000);
        Registry r; json::install(r); r.add(json::property("doc.name.v1", "/name"));
        Database db(r); SQL sql; auto tx = db.begin();
        std::vector<std::string> docs; std::vector<Value> values;
        for (std::size_t i = 0; i < n + 100; ++i) {
            docs.push_back("{\"name\":\"" + (i % 2 ? "item" + std::to_string(i) : "common") + "\",\"nested\":{\"score\":" + std::to_string(i) + "},\"tags\":[\"one\",\"two\"]}");
            values.push_back(json::value(docs.back()));
        }
        sql.exec("BEGIN");
        for (const std::string table : {"scan", "indexed"}) {
            tx.create_table(table, {{"id", integer()}, {"doc", json::type(), false, table == "indexed" ? "doc.name.v1" : ""}});
            sql.exec("CREATE TABLE " + table + "(id INTEGER, doc TEXT CHECK(json_valid(doc)))");
            if (table == "indexed") sql.exec("CREATE INDEX names ON indexed(json_extract(doc,'$.name'))");
            Statement insert(sql, "INSERT INTO " + table + " VALUES (?,?)");
            for (std::size_t i = 0; i < n; ++i) {
                tx.insert(table, {static_cast<std::int64_t>(i), values[i]});
                insert.bind(static_cast<std::int64_t>(i), docs[i]); insert.insert();
            }
        }
        tx.commit(); sql.exec("COMMIT");
        std::cerr << "SQLite " << sqlite3_libversion() << " " << sqlite3_sourceid() << '\n';
        std::cout << "operation,rows,coresql_us,sqlite_us\n";
        for (const std::string table : {"scan", "indexed"}) {
            for (const std::string kind : {"one", "missing", "half"}) {
                const auto target = (n / 2) | 1;
                auto key = kind == "one" ? "item" + std::to_string(target) : kind == "half" ? "common" : "absent";
                std::vector<std::int64_t> expected;
                for (std::size_t i = 0; i < n; ++i)
                    if ((kind == "one" && i == target) || (kind == "half" && i % 2 == 0)) expected.push_back(static_cast<std::int64_t>(i));
                Query query{table, {column("id")}, contains(column("doc"), "doc.name.v1", literal(key))};
                Statement select(sql, "SELECT id FROM " + table + " WHERE json_extract(doc,'$.name')=?");
                check(sqlite3_bind_text(select.p, 1, key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
                auto verify = [&](std::vector<std::int64_t> found) { std::sort(found.begin(), found.end()); check(found == expected); };
                compare(table + "_" + kind, n, table == "scan" || kind == "half" ? 5 : 100,
                    [&] { auto result = db.query(query); std::vector<std::int64_t> found; found.reserve(result.rows.size());
                        for (const auto& row : result.rows) found.push_back(std::get<std::int64_t>(row[0])); verify(std::move(found)); },
                    [&] { std::vector<std::int64_t> found; int rc;
                        while ((rc = sqlite3_step(select.p)) == SQLITE_ROW) found.push_back(sqlite3_column_int64(select.p, 0));
                        check(rc == SQLITE_DONE); check(sqlite3_reset(select.p) == SQLITE_OK); verify(std::move(found)); });
            }
            Statement insert(sql, "INSERT INTO " + table + " VALUES (?,?)");
            compare(table + "_insert100_rollback", n, 10,
                [&] { auto t = db.begin(); for (std::size_t i = n; i < n + 100; ++i) t.insert(table, {static_cast<std::int64_t>(i), values[i]}); t.rollback(); },
                [&] { sql.exec("BEGIN"); for (std::size_t i = n; i < n + 100; ++i) { insert.bind(static_cast<std::int64_t>(i), docs[i]); insert.insert(); } sql.exec("ROLLBACK"); });
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
