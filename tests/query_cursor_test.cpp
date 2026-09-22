#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/query.hpp"
#include <algorithm>
#include <tuple>
using namespace coresql;

int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        Database db(registry);
        sql::Connection sql(db, registry);
        sql.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT)");
        sql.execute("BEGIN");
        for (std::int64_t i = 0; i < 1000; ++i)
            sql.execute("INSERT INTO items VALUES(?, ?)", Row{i, std::string(128, 'x')});
        sql.execute("COMMIT");
        // Opening a cursor binds the query once, including at LIMIT 0.
        // Test the core API: SQL rejects these aliases before reaching it.
        for (bool multi : {false, true})
            for (const auto& aliases :
                 std::vector<std::pair<std::string, std::string>>{{"same", "same"}, {"", "b"}, {"a", ""}})
                for (std::size_t limit : {std::size_t{0}, std::size_t{10}}) {
                    Query invalid{"items", {column(aliases.first, "id")}};
                    invalid.alias = aliases.first;
                    invalid.limit = limit;
                    Join join{"items", aliases.second, column(aliases.first, "id"),
                              column(aliases.second, "id")};
                    if (multi)
                        invalid.joins.push_back(join);
                    else
                        invalid.join = join;
                    expect(ErrorCode::schema, [&] { db.query(invalid); });
                    expect(ErrorCode::schema, [&] { db.cursor(invalid); });
                    std::size_t visited = 0;
                    expect(ErrorCode::schema, [&] {
                        db.query_each(invalid, [&](std::span<const Value>) {
                            ++visited;
                            return true;
                        });
                    });
                    CHECK(visited == 0);
                }
        Query q{"items", {column("id")}};
        detail::QueryCounters counters;
        auto cursor = db.cursor(q);
        {
            detail::QueryCounterScope counted(counters);
            auto first = cursor.fetch(7);
            CHECK(first.types == std::vector<Type>{integer()});
            CHECK(first.rows.size() == 7);
            CHECK(std::get<std::int64_t>(first.rows.back()[0]) == 6);
            CHECK(counters.rows_tested == 7);
        }
        CHECK(cursor.stats().rows == 7 && !cursor.stats().closed);
        CHECK(cursor.stats().snapshot_payload_bytes > 100000);
        CHECK(cursor.fetch(0).rows.empty());
        sql.execute("DELETE FROM items WHERE id=7");
        CHECK(std::get<std::int64_t>(cursor.next()->at(0)) == 7);
        auto moved = std::move(cursor);
        expect(ErrorCode::state, [&] { cursor.next(); });
        moved.close();
        CHECK(!moved.next() && moved.stats().closed && moved.stats().snapshot_payload_bytes == 0);

        auto detached = [] {
            Database owner;
            auto tx = owner.begin();
            tx.create_table("detached", {{"id", integer()}});
            tx.insert("detached", Row{std::int64_t{42}});
            tx.commit();
            return owner.cursor(Query{"detached"});
        }();
        CHECK(detached.fetch(2).rows == std::vector<Row>{{std::int64_t{42}}});

        auto staged = db.begin();
        auto stable = staged.cursor(q);
        staged.erase("items");
        staged.rollback();
        CHECK(stable.fetch(2000).rows.size() == 999);
        CHECK(stable.stats().closed);

        q.offset = 998;
        q.limit = 10;
        CHECK(db.cursor(q).fetch(20).rows == db.query(q).rows);
        q.offset = 0;
        q.limit = 0;
        CHECK(db.cursor(q).fetch(10).rows.empty());
        q.limit = std::numeric_limits<std::size_t>::max();
        QueryOptions limited;
        limited.max_work = 20;
        auto bounded_work = db.cursor(q, limited);
        bounded_work.next();
        expect(ErrorCode::resource, [&] {
            for (int i = 0; i < 50; ++i)
                bounded_work.next();
        });
        CHECK(bounded_work.stats().closed);
        std::stop_source cancellation;
        QueryOptions interrupt;
        interrupt.cancellation = cancellation.get_token();
        auto interrupted = db.cursor(q, interrupt);
        interrupted.next();
        cancellation.request_stop();
        expect(ErrorCode::cancelled, [&] { interrupted.next(); });
        std::stop_source batch_cancellation;
        interrupt.cancellation = batch_cancellation.get_token();
        auto interrupted_batch = db.cursor(q, interrupt);
        batch_cancellation.request_stop();
        expect(ErrorCode::cancelled, [&] { interrupted_batch.fetch(10); });
        CHECK(interrupted_batch.stats().closed && interrupted_batch.fetch(10).rows.empty());

        QueryOptions memory;
        memory.max_buffer_bytes = 1024;
        auto small_batches = db.cursor(Query{"items"}, memory);
        std::size_t total = 0;
        while (auto row = small_batches.next()) {
            CHECK(row->size() == 2);
            ++total;
        }
        CHECK(total == 999 && small_batches.stats().peak_buffer_bytes <= memory.max_buffer_bytes);
        auto oversized_batch = db.cursor(Query{"items"}, memory);
        expect(ErrorCode::resource, [&] { oversized_batch.fetch(100); });
        CHECK(oversized_batch.stats().closed);
        expect(ErrorCode::resource, [&] { db.query(Query{"items"}, memory); });
        Query top{"items", {column("id")}, {}, {{column("id"), true}}, 2};
        memory.max_buffer_bytes = 4096;
        CHECK(db.query(top, memory).rows.size() == 2);
        memory.max_buffer_bytes = 1024;
        expect(ErrorCode::resource,
               [&] { sql.query(sql::Statement("SELECT DISTINCT value,id FROM items"), {}, memory); });
        expect(ErrorCode::resource,
               [&] { sql.query(sql::Statement("SELECT id,count(*) FROM items GROUP BY id"), {}, memory); });
        CHECK(sql.execute("SELECT count(*) FROM items").rows.size() == 1);

        auto compare = [&](const char* text) {
            sql::Statement statement(text);
            const auto expected = sql.execute(statement);
            auto stream = sql.cursor(statement);
            CHECK(stream.columns() == expected.columns);
            std::vector<Row> rows;
            for (;;) {
                auto batch = stream.fetch(3);
                CHECK(batch.columns == expected.columns);
                if (batch.rows.empty())
                    break;
                for (auto& row : batch.rows)
                    rows.push_back(std::move(row));
            }
            CHECK(rows == expected.rows);
        };
        compare("SELECT 42 AS answer, 'hello' AS greeting");
        compare("SELECT 42 LIMIT 1 OFFSET 1");
        compare("SELECT id FROM items WHERE id>=995 LIMIT 3 OFFSET 1");
        compare("SELECT 1 AS n UNION ALL SELECT 2 UNION ALL SELECT 3 LIMIT 2 OFFSET 1");
        compare("SELECT id FROM items WHERE id<3 UNION ALL SELECT 1001 LIMIT 4");
        sql.execute("CREATE TABLE keys(id INTEGER PRIMARY KEY, label TEXT)");
        sql.execute("INSERT INTO keys VALUES(0,'zero'),(2,'two'),(1001,'absent')");
        compare("SELECT a.id,b.label FROM items a JOIN keys b ON a.id=b.id");
        compare("SELECT a.id,b.label FROM items a LEFT JOIN keys b ON a.id=b.id WHERE a.id<4");
        compare("SELECT a.id,b.id FROM keys a CROSS JOIN keys b LIMIT 5 OFFSET 2");
        compare("SELECT a.id,b.id FROM items a,keys b WHERE a.id=b.id AND a.id<3");
        sql::Statement named("SELECT id FROM items WHERE id>=:first LIMIT 2");
        auto bound = sql.cursor(named, Row{std::int64_t{5}});
        CHECK(bound.fetch(2).rows == sql.execute(named, Row{std::int64_t{5}}).rows);
        expect(ErrorCode::unsupported,
               [&] { sql.cursor(sql::Statement("SELECT id FROM items ORDER BY id")); });
        sql.execute("CREATE INDEX items_id ON items(id)");
        compare("SELECT id FROM items ORDER BY id LIMIT 5");
        compare("SELECT id FROM items ORDER BY id DESC LIMIT 4");
        sql.execute("CREATE TABLE mid(a INTEGER, id INTEGER)");
        sql.execute("CREATE TABLE tail(b INTEGER, id INTEGER)");
        sql.execute("INSERT INTO mid VALUES(0,10),(0,11),(2,20)");
        sql.execute("INSERT INTO tail VALUES(10,100),(11,110)");
        {
            sql::Statement statement(
                "SELECT a.id,b.id,c.id FROM items a JOIN mid b ON a.id=b.a JOIN tail c ON b.id=c.b");
            auto expected = sql.execute(statement);
            auto stream = sql.cursor(statement);
            std::vector<Row> rows;
            while (auto row = stream.next())
                rows.push_back(std::move(*row));
            auto by_id = [](const Row& row) {
                return std::tuple{std::get<std::int64_t>(row[0]), std::get<std::int64_t>(row[1]),
                                  std::get<std::int64_t>(row[2])};
            };
            std::sort(expected.rows.begin(), expected.rows.end(),
                      [&](const Row& a, const Row& b) { return by_id(a) < by_id(b); });
            std::sort(rows.begin(), rows.end(),
                      [&](const Row& a, const Row& b) { return by_id(a) < by_id(b); });
            CHECK(rows == expected.rows);
            CHECK(rows.size() == 2);
        }
        compare("SELECT rowid, id FROM items WHERE id<3");
        {
            Query mismatched{"items", {column("id")}};
            mismatched.compounds.push_back(
                {SetOperation::union_all, std::make_shared<Query>(Query{"keys", {column("label")}})});
            expect(ErrorCode::type, [&] { db.cursor(mismatched); });
            Query wider{"items", {column("id")}};
            wider.compounds.push_back(
                {SetOperation::union_all,
                 std::make_shared<Query>(Query{"keys", {column("id"), column("label")}})});
            expect(ErrorCode::type, [&] { db.cursor(wider); });
            Query forward{"items", {column("a", "id")}};
            forward.alias = "a";
            forward.joins = {Join{"mid", "b", column("b", "id"), column("c", "id")},
                             Join{"tail", "c", column("a", "id"), column("c", "b")}};
            expect(ErrorCode::schema, [&] { db.cursor(forward); });
        }
        sql.execute("CREATE TABLE left_a(id INTEGER)");
        sql.execute("CREATE TABLE left_b(a INTEGER, id INTEGER)");
        sql.execute("CREATE TABLE left_c(a INTEGER, id INTEGER)");
        sql.execute("INSERT INTO left_a VALUES(1),(2),(3)");
        sql.execute("INSERT INTO left_b VALUES(1,10)");
        sql.execute("INSERT INTO left_c VALUES(1,100),(2,200)");
        {
            sql::Statement statement("SELECT a.id, b.id, c.id FROM left_a a LEFT JOIN left_b b ON a.id=b.a "
                                     "LEFT JOIN left_c c ON a.id=c.a");
            auto expected = sql.execute(statement);
            auto stream = sql.cursor(statement);
            std::vector<Row> rows;
            while (auto row = stream.next())
                rows.push_back(std::move(*row));
            auto key = [](const Row& row) { return std::get<std::int64_t>(row[0]); };
            std::sort(expected.rows.begin(), expected.rows.end(),
                      [&](const Row& a, const Row& b) { return key(a) < key(b); });
            std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return key(a) < key(b); });
            CHECK(rows == expected.rows);
            CHECK(rows.size() == 3);
            CHECK(is_null(rows[1][1]) && std::get<std::int64_t>(rows[1][2]) == 200);
            CHECK(is_null(rows[2][1]) && is_null(rows[2][2]));
        }
        {
            sql::Statement statement("SELECT a.id FROM left_a a LEFT JOIN left_b b ON a.id=b.a "
                                     "JOIN left_c c ON a.id=c.a");
            auto expected = sql.execute(statement);
            auto stream = sql.cursor(statement);
            std::vector<Row> rows;
            while (auto row = stream.next())
                rows.push_back(std::move(*row));
            auto key = [](const Row& row) { return std::get<std::int64_t>(row[0]); };
            std::sort(expected.rows.begin(), expected.rows.end(),
                      [&](const Row& a, const Row& b) { return key(a) < key(b); });
            std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return key(a) < key(b); });
            CHECK(rows == expected.rows);
            CHECK(rows.size() == 2);
        }
        // Resume a NULL-extended first join more than once while its second
        // input changes in the database. The cursor keeps its original snapshot.
        {
            sql.execute("INSERT INTO left_c VALUES(2,201)");
            sql::Statement statement("SELECT a.id,b.id,c.id FROM left_a a LEFT JOIN left_b b ON a.id=b.a "
                                     "LEFT JOIN left_c c ON a.id=c.a WHERE a.id=2");
            const auto expected = sql.execute(statement);
            CHECK(expected.rows.size() == 2);
            auto stream = sql.cursor(statement);
            auto first = stream.next();
            CHECK(first && *first == expected.rows[0]);
            sql.execute("UPDATE left_c SET id=id+1000 WHERE a=2");
            auto second = stream.next();
            CHECK(second && *second == expected.rows[1]);
            CHECK(!stream.next());
        }
        expect(ErrorCode::unsupported, [&] {
            sql.cursor(sql::Statement("SELECT a.id FROM items a JOIN mid b ON a.id=b.a JOIN tail c ON "
                                      "b.id=c.b JOIN keys d ON a.id=d.id"));
        });
        expect(ErrorCode::unsupported,
               [&] { sql.cursor(sql::Statement("SELECT id FROM items UNION SELECT id FROM keys")); });
        expect(ErrorCode::schema, [&] { sql.cursor(sql::Statement("SELECT missing FROM items LIMIT 0")); });
        // Pausing a cursor pins its current page while other reads churn a tiny cache.
        Database paged(registry, OpenOptions{false, 512});
        sql::Connection paged_sql(paged, registry);
        paged_sql.execute("CREATE TABLE pages(id INTEGER PRIMARY KEY, payload TEXT)");
        paged_sql.execute("BEGIN");
        for (std::int64_t i = 0; i < 24; ++i)
            paged_sql.execute("INSERT INTO pages VALUES(?,?)", Row{i, std::string(2048, char('a' + i))});
        paged_sql.execute("COMMIT");
        auto pages = paged_sql.cursor(sql::Statement("SELECT id,payload FROM pages"));
        CHECK(std::get<std::int64_t>(pages.next()->at(0)) == 0);
        CHECK(paged_sql.execute("SELECT payload FROM pages WHERE id=23").rows[0][0] ==
              Value(std::string(2048, 'x')));
        for (std::int64_t i = 1; i < 24; ++i) {
            auto row = pages.next();
            CHECK(row && row->at(0) == Value(i) && row->at(1) == Value(std::string(2048, char('a' + i))));
        }
        CHECK(!pages.next());
        auto joined_pages = paged_sql.cursor(
            sql::Statement("SELECT a.payload,b.payload FROM pages a JOIN pages b ON a.id=b.id"));
        for (std::int64_t i = 0; i < 24; ++i) {
            auto row = joined_pages.next();
            CHECK(row && row->at(0) == row->at(1) && row->at(0) == Value(std::string(2048, char('a' + i))));
        }
        CHECK(!joined_pages.next());

        std::size_t visited = 0;
        sql.query_each(sql::Statement("SELECT 1 UNION ALL SELECT 2"), [&](auto) { return ++visited != 1; });
        CHECK(visited == 1);
    });
}
