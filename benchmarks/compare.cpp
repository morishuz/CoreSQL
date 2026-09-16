#include "coresql/core.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <string>

using namespace coresql;
using Clock = std::chrono::steady_clock;
struct SQLite {
    sqlite3* db = nullptr;
    SQLite() { if (sqlite3_open(":memory:", &db) != SQLITE_OK) throw std::runtime_error("SQLite open"); }
    ~SQLite() { sqlite3_close(db); }
    void exec(const char* sql) { if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db)); }
};
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(SQLite& db, const char* sql) {
        if (sqlite3_prepare_v2(db.db, sql, -1, &p, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db.db));
    }
    ~Statement() { sqlite3_finalize(p); }
    int step() { int rc = sqlite3_step(p); if (rc != SQLITE_ROW && rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p))); return rc; }
    void reset() { if (sqlite3_reset(p) != SQLITE_OK) throw std::runtime_error("SQLite reset"); }
};
void check(bool yes) { if (!yes) throw std::runtime_error("Benchmark result mismatch"); }
template<class F> double time_ms(F&& fn) {
    auto start = Clock::now(); fn();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
template<class A, class B> void compare(const char* name, std::size_t n, A&& a, B&& b) {
    a(); b(); // untimed warmup, including correctness checks
    std::vector<double> ca, sb;
    for (int i = 0; i < 7; ++i) {
        if (i % 2) { sb.push_back(time_ms(b)); ca.push_back(time_ms(a)); }
        else { ca.push_back(time_ms(a)); sb.push_back(time_ms(b)); }
    }
    std::sort(ca.begin(), ca.end()); std::sort(sb.begin(), sb.end());
    std::cout << name << ',' << n << ',' << ca[3] << ',' << sb[3] << ',' << ca[3] / sb[3] << '\n';
}
int main(int argc, char** argv) {
 try {
    const std::size_t n = argc == 2 ? std::stoull(argv[1]) : 10000;
    check(n >= 100 && n <= 1000000);
    std::cerr << "SQLite " << sqlite3_libversion() << " source=" << sqlite3_sourceid() << '\n';
    std::cerr << "compiler=" << __VERSION__ << "; both in-memory; no indexes; seven alternating repeats; median milliseconds\n";
    std::cerr << "Query rows are materialized into CoreSQL Value containers for both engines; checks included equally. SQLite preparation separately reported.\n";
    std::cout << "workload,rows,coresql_ms,sqlite_ms,coresql_over_sqlite\n";
    Database core;
    SQLite sql;
    sql.exec("CREATE TABLE items(id INTEGER, score REAL, name TEXT) STRICT");
    auto tx = core.begin(); tx.create_table("items", {{"id", integer()}, {"score", real()}, {"name", text()}});
    Statement insert(sql, "INSERT INTO items VALUES(?,?,?)");
    sql.exec("BEGIN");
    for (std::size_t i = 0; i < n; ++i) {
        const auto name = "row" + std::to_string(i);
        tx.insert("items", {static_cast<std::int64_t>(i), double(i % 100), name});
        sqlite3_bind_int64(insert.p, 1, static_cast<sqlite3_int64>(i));
        sqlite3_bind_double(insert.p, 2, double(i % 100));
        sqlite3_bind_text(insert.p, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        check(insert.step() == SQLITE_DONE); insert.reset();
    }
    tx.commit(); sql.exec("COMMIT");
    compare("begin_rollback", n, [&] { for (int i=0;i<20;++i) { auto t=core.begin(); t.rollback(); } },
        [&] { for(int i=0;i<20;++i) { sql.exec("BEGIN"); sql.exec("ROLLBACK"); } });
    compare("begin_commit", n, [&] { for(int i=0;i<20;++i) { auto t=core.begin(); t.commit(); } },
        [&] { for(int i=0;i<20;++i) { sql.exec("BEGIN"); sql.exec("COMMIT"); } });
    auto query_bench = [&](const char* label, Query query, const char* text) {
        auto expected = core.query(query).rows;
        auto collect = [&](Statement& statement) {
            std::vector<Row> rows;
            while (statement.step() == SQLITE_ROW) {
                Row row;
                for (int c=0;c<sqlite3_column_count(statement.p);++c) {
                    switch(sqlite3_column_type(statement.p,c)) {
                    case SQLITE_INTEGER: row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(statement.p,c))); break;
                    case SQLITE_FLOAT: row.emplace_back(sqlite3_column_double(statement.p,c)); break;
                    case SQLITE_TEXT: row.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement.p,c)), static_cast<std::size_t>(sqlite3_column_bytes(statement.p,c)))); break;
                    default: throw std::runtime_error("Unexpected SQL value");
                    }
                }
                rows.push_back(std::move(row));
            }
            statement.reset(); return rows;
        };
        auto verify = [&](const std::vector<Row>& rows) {
            check(rows.size() == expected.size());
            // Keep verification independent of the engine being benchmarked.
            // In particular, do not time public registry validation for every cell.
            auto equal = [](const Value& a, const Value& b) {
                if (a.index() != b.index()) return false;
                if (const auto* v = std::get_if<std::int64_t>(&a)) return *v == std::get<std::int64_t>(b);
                if (const auto* v = std::get_if<double>(&a)) return *v == std::get<double>(b);
                if (const auto* v = std::get_if<std::string>(&a)) return *v == std::get<std::string>(b);
                throw std::runtime_error("Unexpected opaque benchmark value");
            };
            for(std::size_t i=0;i<rows.size();++i) {
                check(rows[i].size() == expected[i].size());
                for(std::size_t j=0;j<rows[i].size();++j) check(equal(rows[i][j],expected[i][j]));
            }
        };
        Statement prepared(sql,text);
        compare(label,n,[&]{verify(core.query(query).rows);},[&]{verify(collect(prepared));});
    };
    query_bench("scan_prepared_sqlite", Query{"items",{column("id"),column("score"),column("name")}}, "SELECT id,score,name FROM items");
    query_bench("filter_1pct_prepared_sqlite", Query{"items",{column("id")},Predicate{column("score"),Compare::less,literal(1.0)}}, "SELECT id FROM items WHERE score<1.0");
    query_bench("filter_50pct_prepared_sqlite", Query{"items",{column("id")},Predicate{column("score"),Compare::less,literal(50.0)}}, "SELECT id FROM items WHERE score<50.0");
    query_bench("sort_prepared_sqlite", Query{"items",{column("id")},{},{Order{column("id"),true}}}, "SELECT id FROM items ORDER BY id DESC");
    query_bench("sort_limit_prepared_sqlite", Query{"items",{column("id")},{},{Order{column("id"),true}},10}, "SELECT id FROM items ORDER BY id DESC LIMIT 10");
    compare("prepare_only_sqlite_vs_empty_bind",n,[&]{for(int i=0;i<100;++i) core.query(Query{"items",{column("id")},{},{},0});},[&]{for(int i=0;i<100;++i) {Statement s(sql,"SELECT id FROM items LIMIT 0");}});
    compare("insert_100_rollback",n,[&]{auto t=core.begin();for(int i=0;i<100;++i)t.insert("items",{std::int64_t{-1},1.0,std::string("added")});t.rollback();},[&]{sql.exec("BEGIN");for(int i=0;i<100;++i) {sqlite3_bind_int64(insert.p,1,-1);sqlite3_bind_double(insert.p,2,1);sqlite3_bind_text(insert.p,3,"added",-1,SQLITE_STATIC);check(insert.step()==SQLITE_DONE);insert.reset();}sql.exec("ROLLBACK");});
    Statement update(sql,"UPDATE items SET name='changed' WHERE id=?");
    sqlite3_bind_int64(update.p,1,static_cast<sqlite3_int64>(n/2));
    compare("update_one_rollback",n,[&]{auto t=core.begin();check(t.update("items",{{"name",literal(std::string("changed"))}},Predicate{column("id"),Compare::equal,literal(static_cast<std::int64_t>(n/2))})==1);t.rollback();},[&]{sql.exec("BEGIN");check(update.step()==SQLITE_DONE);check(sqlite3_changes(sql.db)==1);update.reset();sql.exec("ROLLBACK");});
    Statement erase(sql,"DELETE FROM items WHERE score<1.0");
    compare("delete_1pct_rollback",n,[&]{auto t=core.begin();check(t.erase("items",Predicate{column("score"),Compare::less,literal(1.0)})==(n+99)/100);t.rollback();},[&]{sql.exec("BEGIN");check(erase.step()==SQLITE_DONE);check(static_cast<std::size_t>(sqlite3_changes(sql.db))==(n+99)/100);erase.reset();sql.exec("ROLLBACK");});
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
