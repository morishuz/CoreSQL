#include "check.hpp"
#include "coresql/decimal.hpp"
#include "coresql/sql.hpp"
#include <array>
using namespace coresql;
int main() {
    return tests([] {
        auto t = decimals::type(15, 2);
        Registry native;
        decimals::install(native);
        Database standalone(native);
        auto a = decimals::parse("0.10", t), b = decimals::parse("0.20", t);
        CHECK(decimals::format(
                  standalone.query(Query{"", {call("decimal.add", {literal(a), literal(b)})}}).rows[0][0]) ==
              "0.30");
        for (auto [input, wanted] : {std::pair{"1.235", "1.24"},
                                     {"-1.235", "-1.24"},
                                     {"1.234", "1.23"},
                                     {"-0.004", "0.00"},
                                     {"5e-3", "0.01"},
                                     {"1e-100", "0.00"}})
            CHECK(decimals::format(decimals::parse(input, t, true)) == wanted);
        CHECK(decimals::format(decimals::parse("000001.2300", t)) == "1.23");
        CHECK(decimals::format(decimals::literal("0.0001000")) == "0.0001");
        for (auto input : {"", ".", "--1", " 1", "1 ", "1..0", "1e", "1e-", "NaN", "Infinity", "1x"})
            expect(ErrorCode::type, [&] { decimals::parse(input, t); });
        expect(ErrorCode::type, [&] { decimals::parse("1.231", t); });
        expect(ErrorCode::constraint, [&] { decimals::parse("9.995", decimals::type(3, 2), true); });
        expect(ErrorCode::constraint, [&] { decimals::parse("1e100", t); });
        expect(ErrorCode::constraint,
               [&] { decimals::parse("100000000000000000000000000000000000000", decimals::type(38, 0)); });
        for (Bytes parameters : {Bytes{}, Bytes{std::byte{1}}, Bytes{std::byte{0}, std::byte{0}},
                                 Bytes{std::byte{39}, std::byte{0}}, Bytes{std::byte{1}, std::byte{2}},
                                 Bytes{std::byte{2}, std::byte{1}, std::byte{0}}}) {
            Type malformed{t.id, t.version, parameters};
            expect(ErrorCode::type, [&] { (void)decimals::format_of(malformed); });
            expect(ErrorCode::type, [&] { native.validate(malformed); });
        }
        expect(ErrorCode::type, [&] { decimals::format_of(integer()); });
        expect(ErrorCode::type, [&] { decimals::type(39, 0); });
        expect(ErrorCode::type, [&] { decimals::type(1, 2); });
        expect(ErrorCode::format, [&] { native.validate(Opaque(t, Bytes{}), t); });
        CHECK(decimals::to_integer(decimals::parse("-9223372036854775808", decimals::type(19, 0))) ==
              INT64_MIN);
        expect(ErrorCode::constraint,
               [&] { decimals::to_integer(decimals::parse("9223372036854775808", decimals::type(19, 0))); });
        expect(ErrorCode::type, [&] { decimals::to_integer(decimals::parse("1.10", t)); });

        Registry r;
        sql::install(r);
        TempDirectory temp;
        {
            auto db = Database::open(temp.path / "db", r);
            sql::Connection c(db, r);
            auto one = [&](std::string_view sql) {
                auto rows = c.execute(sql).rows;
                CHECK(rows.size() == 1 && rows[0].size() == 1);
                return rows[0][0];
            };
            auto formatted = [&](std::string_view sql) { return decimals::format(one(sql)); };
            CHECK(formatted("SELECT CAST(0.1 AS DECIMAL(15,2))+CAST(0.2 AS DECIMAL(15,2))") == "0.30");
            CHECK(formatted("SELECT CAST(9007199254740993.01 AS DECIMAL(38,2))") == "9007199254740993.01");
            CHECK(formatted("SELECT CAST(-(-1.25) AS DECIMAL(4,2))") == "1.25");
            CHECK(formatted("SELECT CAST(-(-(-1.25)) AS DECIMAL(4,2))") == "-1.25");
            CHECK(formatted("SELECT CAST('-1.235' AS DECIMAL(5,2))") == "-1.24");
            CHECK(formatted(
                      "SELECT CAST('18000000000000000000000000000000000000' AS "
                      "DECIMAL(38,0))-CAST('9000000000000000000000000000000000000.0' AS DECIMAL(38,1))") ==
                  "9000000000000000000000000000000000000.0");
            CHECK(one("SELECT CAST('9007199254740993' AS DECIMAL(38,0))>9007199254740992") ==
                  Value(std::int64_t{1}));
            CHECK(one("SELECT CAST('-1' AS DECIMAL(2,0))>CAST('-1.01' AS DECIMAL(3,2))") ==
                  Value(std::int64_t{1}));
            CHECK(one("SELECT CAST('0.1' AS DECIMAL(2,1))=CAST('0.10' AS DECIMAL(38,2))") ==
                  Value(std::int64_t{1}));
            CHECK(is_null(one("SELECT CAST('1' AS DECIMAL)/0")));
            CHECK(one("SELECT CAST('1' AS DECIMAL)/2") == Value(0.5));
            CHECK(one("SELECT CAST(CAST('1.9' AS DECIMAL) AS INTEGER)") == Value(std::int64_t{1}));
            CHECK(one("SELECT CAST(CAST('-0.1' AS DECIMAL(2,1)) AS TEXT)") == Value(std::string("-0.1")));
            CHECK(is_null(one("SELECT CAST(NULL AS DECIMAL(15,2))")));
            CHECK(one("SELECT CASE WHEN 0 THEN CAST('invalid' AS DECIMAL(5,2)) ELSE CAST(1 AS DECIMAL(5,2)) "
                      "END") == decimals::parse("1", decimals::type(5, 2)));
            CHECK(one("SELECT CAST('0.1' AS DECIMAL) BETWEEN NULL AND 0") == Value(std::int64_t{0}));
            CHECK(is_null(one("SELECT CAST('0.1' AS DECIMAL)=NULL")));
            CHECK(one("SELECT CASE CAST('0.1' AS DECIMAL(2,1)) WHEN 0.1 THEN 1 ELSE 0 END") ==
                  Value(std::int64_t{1}));
            CHECK(one("SELECT CASE 1 WHEN CAST('1' AS DECIMAL(38,0)) THEN 1 ELSE 0 END") ==
                  Value(std::int64_t{1}));
            c.execute("CREATE TABLE amounts(id INTEGER PRIMARY KEY, x DECIMAL(15,2) UNIQUE, d DECIMAL(15,2) "
                      "DEFAULT 0.01)");
            c.execute("INSERT INTO amounts(id,x) VALUES(1,0.1),(2,0.2),(3,NULL)");
            CHECK(formatted("SELECT sum(x) FROM amounts") == "0.30");
            CHECK(one("SELECT avg(x) FROM amounts") == Value(0.15));
            CHECK(formatted("SELECT sum(x*0.1) FROM amounts") == "0.030");
            CHECK(one("SELECT count(*) FROM amounts WHERE x IN (0.1,0.2)") == Value(std::int64_t{2}));
            CHECK(one("SELECT 0.1 IN (SELECT x FROM amounts)") == Value(std::int64_t{1}));
            CHECK(c.execute("SELECT x FROM amounts WHERE x=CAST(0.1 AS DECIMAL(15,2))").rows.size() == 1);
            CHECK(c.execute("SELECT x FROM amounts WHERE x BETWEEN 0.05 AND 0.15").rows.size() == 1);
            CHECK(c.execute("SELECT CASE WHEN x>0 THEN x ELSE 0 END FROM amounts").rows.size() == 3);
            CHECK(formatted("SELECT COALESCE(NULL,CAST('1.2' AS DECIMAL(3,1)),1)") == "1.2");
            auto original = c.execute("SELECT * FROM amounts ORDER BY id").rows;
            expect(ErrorCode::type, [&] { c.execute("INSERT INTO amounts(id,x) VALUES(4,0.3),(5,0.001)"); });
            expect(ErrorCode::type,
                   [&] { c.execute("UPDATE amounts SET x=CASE WHEN id=1 THEN '1.23' ELSE 'bad' END"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO amounts(id,x) VALUES(4,'0.10')"); });
            CHECK(c.execute("SELECT * FROM amounts ORDER BY id").rows == original);
            c.execute("BEGIN");
            c.execute("UPDATE amounts SET x=-0.1 WHERE id=1");
            c.execute("ROLLBACK");
            CHECK(c.execute("SELECT * FROM amounts ORDER BY id").rows == original);
            c.execute("UPDATE amounts SET x=NULL WHERE id=3");
            c.execute("CREATE TABLE copied(x DECIMAL(18,3))");
            c.execute("INSERT INTO copied SELECT x FROM amounts");
            CHECK(formatted("SELECT sum(x) FROM copied") == "0.300");
            CHECK(c.execute("SELECT a.id FROM amounts a,copied b WHERE a.x=b.x ORDER BY a.id").rows.size() ==
                  2);
            CHECK(c.execute("SELECT x FROM amounts UNION SELECT x FROM copied").rows.size() == 3);
            CHECK(c.execute("SELECT x FROM copied UNION SELECT x FROM amounts").rows.size() == 3);
            CHECK(one("SELECT 0.1 IN (SELECT x FROM amounts UNION SELECT x FROM copied)") ==
                  Value(std::int64_t{1}));
            CHECK(one("SELECT count(*) FROM amounts WHERE x IN (SELECT x FROM copied)") ==
                  Value(std::int64_t{2}));
            CHECK(
                c.execute("SELECT x,(SELECT sum(b.x) FROM copied b WHERE b.x=a.x) FROM amounts a ORDER BY id")
                    .rows.size() == 3);
            CHECK(one("SELECT count(DISTINCT x) FROM amounts") == Value(std::int64_t{2}));
            CHECK(is_null(one("SELECT sum(x) FROM amounts WHERE 0")));
            c.execute("ALTER TABLE amounts ADD COLUMN extra DECIMAL(3,2) DEFAULT 0.25");
            for (auto sql : {"SELECT CAST('99.99' AS "
                             "DECIMAL(4,2))*CAST('99999999999999999999999999999999999999' AS DECIMAL(38,0))",
                             "SELECT sum(x) FROM amounts HAVING "
                             "CAST('99999999999999999999999999999999999999' AS DECIMAL(38,0))+1>0"})
                expect(ErrorCode::constraint, [&] { c.execute(sql); });
            expect(ErrorCode::type, [&] {
                c.execute(
                    "SELECT CAST('0.01' AS DECIMAL(38,38))*CAST('0.1' AS DECIMAL(2,1)) FROM amounts LIMIT 0");
            });
            expect(ErrorCode::type,
                   [&] { c.execute("SELECT CAST(CAST(0.1 AS REAL) AS DECIMAL(5,2)) FROM amounts LIMIT 0"); });
            std::array<Value, 1> parameter{std::string("1.23")};
            sql::Statement prepared("SELECT CAST(? AS DECIMAL(4,2))");
            CHECK(c.execute(prepared, parameter).rows[0][0] == decimals::parse("1.23", decimals::type(4, 2)));
            parameter[0] = std::string("999");
            expect(ErrorCode::constraint, [&] { c.execute(prepared, parameter); });
            c.execute("CREATE TABLE aggregate_edges(x DECIMAL(38,0))");
            c.execute("INSERT INTO aggregate_edges VALUES('99999999999999999999999999999999999999')");
            CHECK(formatted("SELECT sum(x) FROM aggregate_edges") ==
                  "99999999999999999999999999999999999999");
            c.execute("INSERT INTO aggregate_edges VALUES(1),(-1)");
            expect(ErrorCode::constraint, [&] { c.execute("SELECT sum(x) FROM aggregate_edges"); });
            expect(ErrorCode::constraint, [&] { c.execute("SELECT avg(x) FROM aggregate_edges"); });
            c.execute("DELETE FROM aggregate_edges");
            c.execute(
                "INSERT INTO aggregate_edges VALUES('-99999999999999999999999999999999999999'),(-1),(1)");
            expect(ErrorCode::constraint, [&] { c.execute("SELECT sum(x) FROM aggregate_edges"); });
            c.execute("PRAGMA integrity_check");
            db.save(temp.path / "snapshot");
        }
        for (bool snapshot : {false, true}) {
            auto db =
                snapshot ? Database::load(temp.path / "snapshot", r) : Database::open(temp.path / "db", r);
            sql::Connection c(db, r);
            CHECK(decimals::format(c.execute("SELECT sum(x) FROM amounts").rows[0][0]) == "0.30");
            CHECK(decimals::format(c.execute("SELECT extra FROM amounts WHERE id=1").rows[0][0]) == "0.25");
            c.execute("PRAGMA integrity_check");
            if (!snapshot)
                db.checkpoint();
        }
    });
}
