#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/ordered_index.hpp"
#include "../src/query.hpp"
#include <cmath>
using namespace coresql;

int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        TempDirectory temp;
        auto path = temp.path / "covering.core";
        OpenOptions options;
        options.page_cache_bytes = 16384;
        const std::string latest = "SELECT id,ts FROM events WHERE device=1 ORDER BY ts DESC LIMIT 20";
        std::vector<Row> persisted;
        {
            auto db = Database::open(path, registry, options);
            sql::Connection c(db, registry);
            c.execute("CREATE TABLE events(id INTEGER PRIMARY KEY,device INTEGER NOT NULL,ts INTEGER NOT "
                      "NULL,x REAL,label TEXT,body TEXT NOT NULL)");
            c.execute("BEGIN");
            for (std::int64_t i = 0; i < 1000; ++i)
                c.execute("INSERT INTO events VALUES(?,?,?,?,?,?)",
                          Row{2000 - i, i % 2, i, i % 3 ? Value(double(i)) : Value(Null(real())),
                              i % 5 ? Value(std::to_string(i)) : Value(Null(text())),
                              std::string(1024, 'x')});
            c.execute("COMMIT");
            persisted = c.execute(latest).rows;
            c.execute("CREATE INDEX ordered ON events(device,ts)");
            {
                detail::QueryCounters counts;
                detail::QueryCounterScope scope(counts);
                CHECK(c.execute(latest).rows == persisted);
                CHECK(counts.ordered_row_fetches == 20);
                CHECK(counts.ordered_entries <= 21);
            }
            // Prefer the covering index even though a matching narrower index
            // was created first. Index payload columns retain exact stored values.
            c.execute("CREATE INDEX covering ON events(device,ts,id,x,label)");
            db.checkpoint();
            db.trim_cache();
            auto reads = db.cache_stats().page_reads;
            {
                detail::QueryCounters counts;
                detail::QueryCounterScope scope(counts);
                CHECK(c.execute(latest).rows == persisted);
                CHECK(counts.ordered_row_fetches == 0);
                CHECK(counts.ordered_entries <= 21);
                CHECK(c.cursor(sql::Statement(latest)).fetch(100).rows == persisted);
                std::vector<Row> each;
                c.query_each(sql::Statement(latest), [&](std::span<const Value> row) {
                    each.emplace_back(row.begin(), row.end());
                    return true;
                });
                CHECK(each == persisted);
                CHECK(counts.ordered_row_fetches == 0);
            }
            CHECK(db.cache_stats().page_reads == reads);
            const std::string expression = "SELECT id+1,x,label,coalesce(label,'empty') FROM events WHERE "
                                           "device=1 AND ts>500 ORDER BY ts LIMIT 20";
            auto expression_expected = c.execute(expression).rows;
            c.execute("DROP INDEX covering");
            CHECK(c.execute(expression).rows == expression_expected);
            c.execute("CREATE INDEX covering ON events(device,ts,id,x,label)");
            db.trim_cache();
            reads = db.cache_stats().page_reads;
            CHECK(c.execute(expression).rows == expression_expected);
            CHECK(db.cache_stats().page_reads == reads);
            {
                detail::QueryCounters counts;
                detail::QueryCounterScope scope(counts);
                CHECK(c.execute("SELECT body FROM events WHERE device=1 AND ts>500 ORDER BY ts LIMIT 20")
                          .rows.size() == 20);
                CHECK(counts.ordered_row_fetches == 20);
                CHECK(db.cache_stats().page_reads > reads);
            }
            {
                detail::QueryCounters counts;
                detail::QueryCounterScope scope(counts);
                // A non-indexed predicate must fetch candidates, even though
                // every projected column is covered.
                CHECK(c.execute("SELECT id FROM events WHERE device=1 AND body='absent' ORDER BY ts LIMIT 20")
                          .rows.empty());
                CHECK(counts.ordered_row_fetches == 500);
            }
            Query identity{"events",
                           {row_id(), column("id")},
                           Predicate{column("device"), Compare::equal, literal(std::int64_t{1})}};
            identity.order_by = {{column("ts"), true}};
            identity.limit = 2;
            auto ids = db.query(identity).rows;
            CHECK(ids[0][0] != ids[0][1]);
            CHECK(db.cursor(identity).fetch(5).rows == ids);

            auto snapshot = db.begin();
            auto cursor = c.cursor(sql::Statement(latest));
            CHECK(cursor.next() == std::optional<Row>{persisted[0]});
            c.execute("UPDATE events SET id=5000,x=-0.0,label='changed' WHERE id=1001");
            CHECK(c.execute("SELECT id,x,label FROM events WHERE device=1 ORDER BY ts DESC LIMIT 1")
                      .rows[0][0] == Value(std::int64_t{5000}));
            CHECK(std::signbit(std::get<double>(
                c.execute("SELECT x FROM events WHERE device=1 ORDER BY ts DESC LIMIT 1").rows[0][0])));
            c.execute("BEGIN");
            c.execute("SAVEPOINT s");
            c.execute("DELETE FROM events WHERE device=1");
            CHECK(c.execute(latest).rows.empty());
            c.execute("ROLLBACK TO s");
            c.execute("COMMIT");
            c.execute("INSERT OR REPLACE INTO events VALUES(5000,1,1005,NULL,NULL,'new')");
            c.execute("DELETE FROM events WHERE id=1003");
            CHECK(cursor.fetch(100).rows == std::vector<Row>(persisted.begin() + 1, persisted.end()));
            Query old{"events",
                      {column("id"), column("ts")},
                      Predicate{column("device"), Compare::equal, literal(std::int64_t{1})}};
            old.order_by = {{column("ts"), true}};
            old.limit = 20;
            CHECK(snapshot.query(old).rows == persisted);
            persisted = c.execute(latest).rows;
            db.begin().integrity_check();
            db.checkpoint();
        }
        {
            auto db = Database::open(path, registry, options);
            sql::Connection c(db, registry);
            db.trim_cache();
            const auto reads = db.cache_stats().page_reads;
            CHECK(c.execute(latest).rows == persisted);
            CHECK(db.cache_stats().page_reads == reads);
            db.begin().integrity_check();
        }
        // Query-local row selections must be honored even by index-only reads.
        using namespace detail;
        using namespace detail::execution;
        Tables tables;
        auto name = materialize(tables, Result{{integer(), integer()},
                                               {{std::int64_t{1}, std::int64_t{9}},
                                                {std::int64_t{2}, std::int64_t{8}},
                                                {std::int64_t{3}, std::int64_t{7}}}});
        auto table = tables.at(name);
        auto index =
            std::make_shared<OrderedIndex>(IndexDefinition{"selected", {"0", "1"}}, table->columns, registry);
        auto chunk = table->chunks.begin()->second.pin();
        for (std::uint32_t i = 0; i < 3; ++i)
            index->insert(chunk->rows[i], {0, i});
        table->ordered.push_back(index);
        table->selection = std::vector<RowLocation>{{0, 1}};
        Query selected{name, {column("1")}};
        selected.order_by = {{column("0"), true}};
        CHECK(run(tables, selected, registry).rows == std::vector<Row>{{std::int64_t{8}}});
        CHECK(make_cursor(tables, registry, selected).fetch(10).rows == std::vector<Row>{{std::int64_t{8}}});
    });
}
