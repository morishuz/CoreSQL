#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/state.hpp"
using namespace coresql;

int main() {
    return tests([] {
        Registry registry;
        std::vector<std::int64_t> calls;
        registry.add(Function{"observe", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> values) -> Value {
                                  calls.push_back(std::get<std::int64_t>(values[0]));
                                  return std::int64_t{1};
                              }});
        registry.add(Function{
            "fail", [](std::span<const Type>) { return integer(); },
            [](std::span<const Value>) -> Value { throw Error(ErrorCode::constraint, "intentional"); }});
        sql::install(registry);
        Database db(registry);
        sql::Connection sql(db, registry);
        sql.execute("CREATE TABLE a(id INTEGER PRIMARY KEY,k INTEGER NOT NULL,x REAL NOT NULL,n INTEGER)");
        sql.execute("CREATE TABLE b(id INTEGER PRIMARY KEY)");
        sql.execute("INSERT INTO b VALUES(0),(1)");
        sql.execute("BEGIN");
        for (std::int64_t i = 0; i < 4096; ++i)
            sql.execute("INSERT INTO a VALUES(?,?,?,?)",
                        Row{i, i % 2, double(i), i == 2001 ? Value(Null(integer())) : Value(i)});
        sql.execute("COMMIT");
        sql.execute("CREATE INDEX ax ON a(k,x DESC)");
        sql.execute("CREATE INDEX an ON a(n)");
        Query q{"a", {column("a", "id"), column("b", "id")}};
        q.alias = "a";
        q.joins = {{"b", "b", column("a", "k"), column("b", "id")}};
        auto by = [](std::int64_t id) { return Predicate{column("a", "id"), Compare::equal, literal(id)}; };
        auto check = [&](Predicate predicate, std::size_t count, std::size_t maximum) {
            q.where =
                all_of({std::move(predicate),
                        {call("observe", {column("a", "id")}), Compare::equal, literal(std::int64_t{1})}});
            q.repeatable = false;
            calls.clear();
            const auto expected = db.query(q).rows;
            const auto expected_calls = calls;
            calls.clear();
            q.repeatable = true;
            detail::QueryCounters counters;
            {
                detail::QueryCounterScope scope(counters);
                CHECK(db.query(q).rows == expected);
            }
            CHECK(expected.size() == count && calls == expected_calls);
            CHECK(counters.rows_tested <= maximum);
            CHECK(db.cursor(q).fetch(5000).rows == expected);
        };
        check(by(2001), 1, 4);
        check(by(-1), 0, 0);
        check({literal(std::int64_t{2001}), Compare::equal, column("a", "id")}, 1, 4);
        check(all_of({{column("a", "id"), Compare::greater_equal, literal(std::int64_t{2000})},
                      {column("a", "id"), Compare::less, literal(std::int64_t{2010})}}),
              10, 32);
        check(all_of({{column("a", "k"), Compare::equal, literal(std::int64_t{1})},
                      {column("a", "x"), Compare::greater, literal(2000.0)},
                      {column("a", "x"), Compare::less, literal(2010.0)}}),
              5, 20);
        // Overlapping OR arms must not multiply matches or reorder callbacks.
        q.where = any_of({by(2001), by(2001), by(2003)});
        q.repeatable = false;
        auto expected = db.query(q).rows;
        q.repeatable = true;
        detail::QueryCounters disjunction;
        {
            detail::QueryCounterScope scope(disjunction);
            CHECK(db.query(q).rows == expected && expected.size() == 2);
        }
        CHECK(disjunction.rows_tested <= 8);
        // A nullable guard cannot use ordinary WHERE candidate semantics.
        const Predicate nullable{column("a", "n"), Compare::equal, literal(std::int64_t{-1})};
        const Predicate fail{call("fail", {}), Compare::equal, literal(std::int64_t{1})};
        for (bool repeatable : {false, true}) {
            q.repeatable = repeatable;
            q.where = all_of({nullable, fail});
            expect(ErrorCode::constraint, [&] { db.query(q); });
            q.where = any_of({by(-1), all_of({nullable, fail})});
            expect(ErrorCode::constraint, [&] { db.query(q); });
            q.where = all_of({fail, by(-1)});
            expect(ErrorCode::constraint, [&] { db.query(q); });
        }
        q.where = by(2001);
        auto snapshot = db.begin();
        const auto old = snapshot.query(q).rows;
        sql.execute("DELETE FROM a WHERE id=2001");
        sql.execute("INSERT INTO a VALUES(2001,0,2001,NULL)");
        CHECK(snapshot.query(q).rows == old);
        CHECK(db.query(q).rows != old);
        q.limit = 0;
        q.select = {column("a", "missing")};
        expect(ErrorCode::schema, [&] { db.query(q); });
    });
}
