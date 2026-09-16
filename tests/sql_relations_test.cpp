#include "check.hpp"
#include "../src/state.hpp"
#include "coresql/sql.hpp"
#include "coresql/decimal.hpp"
#include "coresql/date.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry r;
        std::size_t calls = 0;
        std::vector<std::int64_t> order;
        r.add(Function{"mark", [](std::span<const Type>) { return integer(); },
                       [&](std::span<const Value> v) -> Value {
                           order.push_back(std::get<std::int64_t>(v[0]));
                           return v[0];
                       }});
        r.add(Function{"tick", [](std::span<const Type>) { return integer(); },
                       [&](std::span<const Value>) -> Value {
                           ++calls;
                           return std::int64_t{7};
                       }});
        r.add(Function{
            "fail", [](std::span<const Type>) { return integer(); },
            [](std::span<const Value>) -> Value { throw Error(ErrorCode::constraint, "intentional"); }});
        sql::install(r);
        Database db(r);
        sql::Connection c(db, r);
        auto one = [&](std::string_view text) { return c.execute(text).rows.at(0).at(0); };
        auto integer_result = [&](std::string_view text, std::int64_t value) {
            CHECK(one(text) == Value(value));
        };
        {
            Database update_db(r);
            sql::Connection c(update_db, r);
            c.execute("CREATE TABLE updates(a INTEGER,b INTEGER,d DATE,n DECIMAL(8,2))");
            c.execute("INSERT INTO updates VALUES(1,2,'2000-01-01',1.25)");
            c.execute("UPDATE updates SET a=b,b=a,d=NULL,n=2.50");
            CHECK(c.execute("SELECT a,b,d,CAST(n AS TEXT) FROM updates").rows ==
                  std::vector<Row>(
                      {{std::int64_t{2}, std::int64_t{1}, Null(dates::type()), std::string("2.50")}}));
            auto unchanged = c.execute("SELECT * FROM updates").rows;
            // Target resolution deliberately precedes lowering its value; WHERE still binds first.
            expect(ErrorCode::schema, [&] { c.execute("UPDATE updates SET missing=missing_function()"); });
            expect(ErrorCode::type,
                   [&] { c.execute("UPDATE updates SET missing=1 WHERE missing_function()=1"); });
            expect(ErrorCode::type, [&] { c.execute("UPDATE updates SET a=7,d='invalid'"); });
            CHECK(c.execute("SELECT * FROM updates").rows == unchanged);
        }
        c.execute("CREATE TABLE t(a INTEGER,b INTEGER)");
        c.execute("INSERT INTO t VALUES(1,2),(2,3),(2,NULL)");
        integer_result("SELECT sum(x) FROM (SELECT a+1 AS x FROM t) AS d", 8);
        integer_result("SELECT sum(x) FROM (SELECT a FROM t) AS d(x)", 5);
        integer_result("SELECT sum(x) FROM t AS d(x,y)", 5);
        integer_result("SELECT count(*) FROM (SELECT a,count(*) AS n FROM t GROUP BY a) AS d", 2);
        integer_result("SELECT count(*) FROM (SELECT a FROM t WHERE 0) AS d", 0);
        CHECK(c.execute("SELECT * FROM (SELECT a,b FROM t) AS d ORDER BY a,b").rows ==
              c.execute("SELECT a,b FROM t ORDER BY a,b").rows);
        integer_result("WITH x(v) AS (SELECT a FROM t), y AS (SELECT v+1 AS n FROM x) SELECT sum(n) FROM y",
                       8);
        integer_result("WITH x AS (SELECT 1 AS a) SELECT (WITH x AS (SELECT 2 AS a) SELECT a FROM x) FROM x",
                       2);
        integer_result("WITH t AS (SELECT 4 AS a) SELECT a FROM t", 4);
        integer_result("WITH x AS (SELECT tick() AS v) SELECT a.v+b.v+(SELECT v FROM x) FROM x AS a,x AS b",
                       21);
        CHECK(calls == 1);
        integer_result("WITH a AS (SELECT mark(1) AS v),b AS (SELECT mark(2) AS v) SELECT b.v+a.v FROM b,a",
                       3);
        CHECK(order == std::vector<std::int64_t>({1, 2}));
        expect(ErrorCode::constraint, [&] {
            c.execute("WITH x AS (SELECT fail() AS v) SELECT CASE WHEN 0 THEN (SELECT v FROM x) ELSE 1 END");
        });
        integer_result("WITH unused AS (SELECT fail() AS v) SELECT 1", 1);
        CHECK(c.execute("WITH x AS (SELECT fail() AS v) SELECT v FROM x LIMIT 0").rows.empty());
        integer_result("SELECT count(*) FROM (SELECT a AS x FROM t GROUP BY x) AS d", 2);
        CHECK(
            c.execute("WITH x AS (SELECT a FROM t) SELECT a FROM x UNION ALL SELECT a FROM x").rows.size() ==
            6);
        CHECK(one("SELECT sum(v) FROM (SELECT 1 AS v UNION ALL SELECT 2.5 AS v) AS d") == Value(3.5));
        for (auto sql : {"SELECT * FROM (SELECT 1 AS a,2 AS a) AS d",
                         "SELECT * FROM (SELECT 1 AS a) AS d(x,y)", "SELECT rowid FROM (SELECT 1 AS a) AS d",
                         "WITH x AS (SELECT 1 AS a),x AS (SELECT 2 AS a) SELECT * FROM x",
                         "SELECT * FROM t,(SELECT t.a AS x) AS d"}) {
            bool failed = false;
            try {
                c.execute(sql);
            } catch (const Error&) {
                failed = true;
            }
            CHECK(failed);
        }
        for (auto sql :
             {"WITH RECURSIVE x AS (SELECT 1) SELECT * FROM x", "WITH x AS (SELECT * FROM x) SELECT * FROM x",
              "WITH x AS (SELECT * FROM y),y AS (SELECT 1) SELECT * FROM x"})
            expect(ErrorCode::unsupported, [&] { c.execute(sql); });
        integer_result("SELECT EXTRACT(YEAR FROM DATE '2000-02-29')", 2000);
        CHECK(is_null(one("SELECT EXTRACT(YEAR FROM NULL)")));
        CHECK(one("SELECT SUBSTRING('aé🙂z' FROM 2 FOR 2)") == Value(std::string("é🙂")));
        CHECK(one("SELECT SUBSTRING('abcd' FROM 0 FOR 2)") == Value(std::string("a")));
        CHECK(one("SELECT SUBSTRING('abcd' FROM -2 FOR 4)") == Value(std::string("a")));
        CHECK(one("SELECT SUBSTRING('abcd' FROM 3)") == Value(std::string("cd")));
        CHECK(is_null(one("SELECT SUBSTRING(NULL FROM 1 FOR 2)")));
        CHECK(c.execute("SELECT SUBSTRING(? FROM 2 FOR 2)", Row{std::string("a\0b", 3)}).rows[0][0] ==
              Value(std::string("\0b", 2)));
        expect(ErrorCode::constraint,
               [&] { c.execute("SELECT SUBSTRING(? FROM 1)", Row{std::string("\xc0\x80", 2)}); });
        CHECK(c.execute("SELECT a,(WITH x AS (SELECT t.a AS v) SELECT v FROM x) FROM t ORDER BY a").rows ==
              c.execute("SELECT a,a FROM t ORDER BY a").rows);
        expect(ErrorCode::constraint, [&] { c.execute("SELECT SUBSTRING('x' FROM 1 FOR -1)"); });
        integer_result("SELECT p.a FROM t AS p,t AS q WHERE p.a IN (SELECT '1' UNION ALL SELECT '1e9999') "
                       "AND p.a=q.a LIMIT 1",
                       1);
        detail::visited_chunks() = 0;
        c.execute("SELECT a FROM t");
        auto chunks = detail::visited_chunks();
        detail::visited_chunks() = 0;
        c.execute("SELECT a IN (SELECT a FROM t) FROM t");
        CHECK(detail::visited_chunks() == 2 * chunks);
        calls = 0;
        c.execute("SELECT a IN (SELECT tick()) FROM t");
        CHECK(calls == 3);
        c.execute("SELECT CASE WHEN 0 THEN a IN (SELECT fail()) ELSE 0 END FROM t");
        c.execute("CREATE TABLE empty(a INTEGER)");
        CHECK(c.execute("SELECT t.a FROM t,empty WHERE t.a IN (SELECT fail()) AND t.a=empty.a").rows.empty());
        c.execute("CREATE TABLE orders(k INTEGER,c INTEGER)");
        c.execute("CREATE TABLE customer(k INTEGER)");
        c.execute("CREATE TABLE lineitem(k INTEGER,q DECIMAL(15,2))");
        c.execute("INSERT INTO customer VALUES(1),(2)");
        c.execute("INSERT INTO orders VALUES(10,1),(20,2)");
        c.execute("INSERT INTO lineitem VALUES(10,200),(10,150),(20,10)");
        auto q18 = "SELECT orders.k,sum(q) FROM customer,orders,lineitem WHERE orders.k IN "
                   "(SELECT k FROM lineitem GROUP BY k HAVING sum(q)>300) AND customer.k=orders.c "
                   "AND orders.k=lineitem.k GROUP BY orders.k ORDER BY orders.k";
        auto rows = c.execute(q18).rows;
        CHECK(rows.size() == 1 && rows[0][0] == Value(std::int64_t{10}));
        CHECK(decimals::format(rows[0][1]) == "350.00");
        c.execute("DELETE FROM lineitem WHERE k=10");
        CHECK(c.execute(q18).rows.empty()); // No cache survives an execution/mutation boundary.
        sql::Statement prepared("WITH x AS (SELECT ? AS a) SELECT a FROM x");
        CHECK(c.execute(prepared, Row{std::int64_t{4}}).rows[0][0] == Value(std::int64_t{4}));
        CHECK(c.execute(prepared, Row{std::int64_t{9}}).rows[0][0] == Value(std::int64_t{9}));
        c.execute("BEGIN");
        c.execute("INSERT INTO t SELECT x,8 FROM (SELECT 20 AS x) AS d");
        integer_result("WITH x AS (SELECT a FROM t) SELECT max(a) FROM x", 20);
        c.execute("ROLLBACK");
        integer_result("SELECT max(a) FROM t", 2);
        std::string nested = "SELECT 1 AS a";
        for (int i = 0; i < 65; ++i)
            nested = "SELECT a FROM (" + nested + ") AS d";
        expect(ErrorCode::unsupported, [&] { c.execute(nested); });
        CHECK(db.schema().size() == 5); // Query-local relations never reach database state.
    });
}
