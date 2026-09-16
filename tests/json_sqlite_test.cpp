#include "check.hpp"
#include "coresql/json.hpp"
#include <sqlite3.h>
#include <random>
#include <algorithm>
using namespace coresql;
namespace {
struct Sqlite {
    sqlite3* db = nullptr;
    Sqlite() { CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK); }
    ~Sqlite() { sqlite3_close(db); }
    void exec(const char* sql) { CHECK(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK); }
};
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(Sqlite& s, const char* sql) { CHECK(sqlite3_prepare_v2(s.db, sql, -1, &p, nullptr) == SQLITE_OK); }
    ~Statement() { sqlite3_finalize(p); }
    void text(int i, const std::string& s) { CHECK(sqlite3_bind_text(p, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT) == SQLITE_OK); }
    void reset() { CHECK(sqlite3_reset(p) == SQLITE_OK); }
};
}
int main() { return tests([] {
    Registry r; json::install(r); r.add(json::property("nested.name.v1", "/nested/name"));
    Database db(r); auto tx = db.begin();
    tx.create_table("scan", {{"id", integer()}, {"doc", json::type()}});
    tx.create_table("indexed", {{"id", integer()}, {"doc", json::type(), false, "nested.name.v1"}});
    Sqlite sql; sql.exec("CREATE TABLE docs(id,doc)");
    Statement insert(sql, "INSERT INTO docs VALUES (?,?)");
    std::mt19937 random(174);
    const std::vector<std::string> values = {R"("alpha")", R"("\u03b2eta")", R"("with\u0000nul")", "null", "false", "17", "{}", "[]"};
    for (std::int64_t i = 0; i < 900; ++i) {
        auto document = i % 13 == 0 ? "{}" : "{\"nested\":{\"name\":" + values[random() % values.size()] + "},\"noise\":[1,-3.5e2,true]}";
        auto value = json::value(document);
        tx.insert("scan", {i, value}); tx.insert("indexed", {i, value});
        CHECK(sqlite3_bind_int64(insert.p, 1, i) == SQLITE_OK); insert.text(2, document);
        CHECK(sqlite3_step(insert.p) == SQLITE_DONE); insert.reset();
    }
    tx.commit(); sql.exec("CREATE INDEX names ON docs(json_extract(doc,'$.nested.name'))");
    Statement select(sql, "SELECT id FROM docs WHERE json_type(doc,'$.nested.name')='text' AND json_extract(doc,'$.nested.name')=? ORDER BY id");
    for (const auto& key : {std::string("alpha"), std::string("βeta"), std::string("with\0nul", 8), std::string("absent")}) {
        select.text(1, key); std::vector<std::int64_t> expected; int rc;
        while ((rc = sqlite3_step(select.p)) == SQLITE_ROW) expected.push_back(sqlite3_column_int64(select.p, 0));
        CHECK(rc == SQLITE_DONE); select.reset();
        for (const std::string table : {"scan", "indexed"}) {
            auto found = db.query({table, {column("id")}, contains(column("doc"), "nested.name.v1", literal(key))});
            std::vector<std::int64_t> actual; for (const auto& row : found.rows) actual.push_back(std::get<std::int64_t>(row[0]));
            std::sort(actual.begin(), actual.end()); CHECK(actual == expected);
        }
    }
    // Seeded byte mutations: anything our stricter parser accepts must be valid JSON to SQLite.
    Statement valid(sql, "SELECT json_valid(?)");
    const std::string base = R"({"a":[1,-2.4e+3,true,null,"\uD83D\uDE80"],"b":{"c":"x"}})";
    for (unsigned i = 0; i < 5000; ++i) {
        auto mutated = base;
        for (unsigned n = 0; n < 1 + i % 3; ++n) {
            auto pos = random() % mutated.size();
            if (i % 3 == 0) mutated.erase(pos, 1);
            else mutated[pos] = static_cast<char>(random() % 256);
        }
        bool accepted = true;
        try { json::value(mutated); } catch (const Error& e) { CHECK(e.code == ErrorCode::type); accepted = false; }
        if (accepted) { valid.text(1, mutated); CHECK(sqlite3_step(valid.p) == SQLITE_ROW); CHECK(sqlite3_column_int(valid.p, 0) == 1); valid.reset(); }
    }
}); }
