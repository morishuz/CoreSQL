#include "check.hpp"
#include "../src/state.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        registry.add(Function{
            "fail", [](std::span<const Type>) { return integer(); },
            [](std::span<const Value>) -> Value { throw Error(ErrorCode::constraint, "intentional"); }});
        sql::install(registry);
        Database db(registry);
        sql::Connection sql(db, registry);
        sql.execute("CREATE TABLE a(k INTEGER, keep INTEGER)");
        sql.execute("CREATE TABLE b(k INTEGER)");
        sql.execute("CREATE TABLE c(k INTEGER)");
        sql.execute("INSERT INTO a VALUES(1,0),(1,1),(1,1),(2,NULL),(3,1)");
        sql.execute("INSERT INTO b VALUES(1),(1),(2),(3)");
        sql.execute("INSERT INTO c VALUES(1),(2),(3)");
        Query q{"a", {column("a", "k"), column("a", "keep"), column("b", "k")}, {}, {}};
        q.alias = "a";
        q.joins = {{"b", "b", literal(std::int64_t{0}), literal(std::int64_t{0}), true},
                   {"c", "c", literal(std::int64_t{0}), literal(std::int64_t{0}), true}};
        q.where = all_of({{column("a", "k"), Compare::equal, column("c", "k")},
                          all_of({{column("b", "k"), Compare::equal, column("c", "k")},
                                  {column("a", "keep"), Compare::equal, literal(std::int64_t{1})}})});
        auto run = [&](const Query& query, detail::QueryCounters& counters) {
            detail::QueryCounterScope scope(counters);
            return db.query(query);
        };
        detail::QueryCounters before, after;
        auto expected = run(q, before);
        q.repeatable = true;
        auto actual = run(q, after);
        CHECK(actual.rows == expected.rows && actual.rows.size() == 5);
        CHECK(after.largest_intermediate < before.largest_intermediate);
        CHECK(after.candidate_pairs < before.candidate_pairs);
        q.limit = 1;
        CHECK(db.query(q).rows == std::vector<Row>{expected.rows.front()});
        q.limit = std::numeric_limits<std::size_t>::max();
        Query direct{"a", {column("a", "k"), column("b", "k")}, {}, {}};
        direct.alias = "a";
        direct.joins = {{"b", "b", column("a", "k"), column("b", "k")}};
        auto joined = db.query(direct);
        direct.repeatable = true;
        detail::QueryCounters borrowed;
        CHECK(run(direct, borrowed).rows == joined.rows);
        CHECK(borrowed.intermediate_rows == 0);
        direct.limit = 1;
        CHECK(db.query(direct).rows == std::vector<Row>{joined.rows.front()});
        // NULL is not FALSE: the suffix must still execute for the NULL keep row.
        q.where->children.push_back({call("fail", {}), Compare::equal, literal(std::int64_t{1})});
        q.where->children[1].children[1].right = literal(std::int64_t{7});
        expect(ErrorCode::constraint, [&] { db.query(q); });
        q.limit = 0;
        detail::QueryCounters zero;
        CHECK(run(q, zero).rows.empty());
        CHECK(zero.candidate_pairs == 0 && zero.rows_tested == 0);
        auto invalid = q;
        invalid.select = {column("a", "missing")};
        expect(ErrorCode::schema, [&] { db.query(invalid); });
        q.limit = 1;
        expect(ErrorCode::constraint, [&] { db.query(q); });
        sql.execute("DELETE FROM c");
        CHECK(db.query(q).rows.empty());
        sql.execute("INSERT INTO c VALUES(1),(2),(3)");
        sql.execute("CREATE TABLE tags(k INTEGER,label TEXT)");
        sql.execute("INSERT INTO tags VALUES(1,'skip'),(2,'keep'),(3,NULL)");
        Query tags{"a", {column("a", "k")}, {}, {}};
        tags.alias = "a";
        tags.joins = {{"tags", "t", column("a", "k"), column("t", "k")}};
        tags.repeatable = true;
        tags.where = all_of({{column("t", "label"), Compare::equal, literal(std::string("absent"))},
                             {call("fail", {}), Compare::equal, literal(std::int64_t{1})}});
        expect(ErrorCode::constraint, [&] { db.query(tags); }); // UNKNOWN must retain the NULL row.
        sql.execute("UPDATE tags SET label='skip'");
        detail::QueryCounters empty_right;
        CHECK(run(tags, empty_right).rows.empty());
        CHECK(empty_right.candidate_pairs == 0 && empty_right.hash_build_rows == 0);
        std::reverse(tags.where->children.begin(), tags.where->children.end());
        expect(ErrorCode::constraint, [&] { db.query(tags); }); // Do not move a filter across fail().
        sql.execute("ALTER TABLE b ADD COLUMN payload TEXT DEFAULT 'retained capture'");
        auto captures = sql.execute("SELECT a.k,(SELECT length(b.payload)) FROM a,b,c "
                                    "WHERE a.k=b.k AND b.k=c.k ORDER BY a.k");
        CHECK(captures.rows.size() == 8);
        for (const auto& row : captures.rows)
            CHECK(row[1] == Value(std::int64_t{16}));
        // A failing expression ahead of a FALSE comparison is never moved behind it.
        q.where = all_of({{call("fail", {}), Compare::equal, literal(std::int64_t{1})},
                          {column("a", "keep"), Compare::equal, literal(std::int64_t{7})}});
        expect(ErrorCode::constraint, [&] { db.query(q); });
        // Exercise SQL lowering with a later scalar subquery stopping its reorder rule.
        const std::string text = "SELECT a.k,b.k FROM a,b,c WHERE a.k=c.k AND b.k=c.k "
                                 "AND a.keep=1 AND (SELECT 1)=1 ORDER BY a.k,b.k";
        detail::QueryCounters lowered;
        {
            detail::QueryCounterScope scope(lowered);
            CHECK(sql.execute(text).rows.size() == 5);
        }
        CHECK(lowered.largest_intermediate < before.largest_intermediate);
        sql.execute("CREATE TABLE x(k INTEGER)");
        sql.execute("CREATE TABLE y(k INTEGER)");
        sql.execute("CREATE TABLE z(k INTEGER)");
        sql.execute("INSERT INTO x VALUES(NULL),(1),(2)");
        sql.execute("INSERT INTO y VALUES(2),(3)");
        sql.execute("INSERT INTO z VALUES(3),(4)");
        Query mixed{"x", {column("x", "k")}};
        mixed.alias = "x";
        mixed.joins = {{"y", "y", literal(std::int64_t{0}), literal(std::int64_t{0}), true},
                       {"z", "z", literal(std::int64_t{0}), literal(std::int64_t{0}), true}};
        mixed.where = all_of({{column("x", "k"), Compare::less, column("y", "k")},
                              {column("y", "k"), Compare::less, column("z", "k")}});
        mixed.source_where = Predicate{literal(std::int64_t{1}), Compare::equal, literal(std::int64_t{1})};
        auto baseline = db.query(mixed);
        mixed.repeatable = true;
        CHECK(db.query(mixed).rows == baseline.rows);
        CHECK(baseline.rows.size() == 4);
        // UNKNOWN from a NULL integer key must reach a later potentially failing conjunct.
        mixed.where->children.push_back({call("fail", {}), Compare::equal, literal(std::int64_t{1})});
        for (bool source_filter : {false, true}) {
            if (!source_filter)
                mixed.source_where.reset();
            else
                mixed.source_where =
                    Predicate{literal(std::int64_t{1}), Compare::equal, literal(std::int64_t{1})};
            for (bool repeatable : {false, true}) {
                mixed.repeatable = repeatable;
                expect(ErrorCode::constraint, [&] { db.query(mixed); });
            }
        }
        // A FALSE local final-WHERE guard cannot hide failure of the source filter.
        mixed.where = all_of({{column("x", "k"), Compare::less, column("x", "k")}});
        mixed.source_where = Predicate{call("fail", {}), Compare::equal, literal(std::int64_t{1})};
        expect(ErrorCode::constraint, [&] { db.query(mixed); });
    });
}
