#include "check.hpp"
#include "coresql/date.hpp"
#include "coresql/encoding.hpp"
#include "coresql/sql.hpp"
#include <array>
using namespace coresql;
int main() {
    return tests([] {
        Registry native;
        dates::install(native);
        Database standalone(native);
        auto parsed = call("date.parse", {literal(std::string("1970-01-01"))});
        CHECK(standalone.query(Query{"", {call("date.format", {parsed})}}).rows[0][0] ==
              Value(std::string("1970-01-01")));
        CHECK(standalone.query(Query{"", {call("date.parse", {literal(Null(text()))})}}).rows[0][0] ==
              Value(Null(dates::type())));
        Registry r;
        sql::install(r);
        CHECK(dates::days(dates::parse("1970-01-01")) == 0);
        CHECK(dates::days(dates::parse("1969-12-31")) == -1);
        CHECK(dates::days(dates::parse("2000-01-01")) == 10957);
        for (auto text : {"0001-01-01", "1582-10-10", "1900-02-28", "2000-02-29", "9999-12-31"}) {
            auto v = dates::parse(text);
            r.validate(v, dates::type());
            CHECK(dates::format(dates::value(dates::days(v))) == text);
        }
        // A full Gregorian leap-year cycle: consecutive encodings remain ordered
        // and every formatted date round-trips through the strict parser.
        auto start = dates::days(dates::parse("1600-01-01"));
        for (std::int64_t i = 0; i < 146097; ++i) {
            auto v = dates::value(start + i);
            CHECK(dates::days(dates::parse(dates::format(v))) == start + i);
        }
        for (auto text : {"", "0000-01-01", "10000-01-01", "1900-02-29", "2100-02-29", "2024-04-31",
                          "2024-00-01", "2024-13-01", "2024-01-00", "2024-01-32", "2024-1-01", "2024-01-1",
                          " 2024-01-01", "2024-01-01 ", "2024-01-01T00:00:00", "202x-01-01"})
            expect(ErrorCode::type, [&] { dates::parse(text); });
        expect(ErrorCode::type, [&] { dates::value(INT64_MIN); });
        expect(ErrorCode::type, [&] { dates::value(INT64_MAX); });
        expect(ErrorCode::type, [&] { dates::days(std::int64_t{0}); });
        expect(ErrorCode::type, [&] { dates::format(Null(dates::type())); });
        auto bad_type = dates::type();
        bad_type.parameters.push_back(std::byte{0});
        expect(ErrorCode::type, [&] { r.validate(bad_type); });
        Bytes malformed;
        encoding::u64(malformed, UINT64_MAX / 2);
        expect(ErrorCode::type, [&] { r.validate(Opaque(dates::type(), malformed), dates::type()); });
        expect(ErrorCode::format, [&] { r.validate(Opaque(dates::type(), Bytes{}), dates::type()); });
        CHECK(r.compare(dates::parse("1969-12-31"), dates::parse("1970-01-01")) < 0);

        TempDirectory temp;
        {
            auto db = Database::open(temp.path / "dates", r);
            sql::Connection c(db, r);
            c.execute("CREATE TABLE events(id BIGINT PRIMARY KEY, happened DATE UNIQUE, fallback DATE "
                      "DEFAULT '2000-02-29')");
            c.execute(
                "INSERT INTO events(id,happened) VALUES(1,'1970-01-01'),(2,DATE '1969-12-31'),(3,NULL)");
            CHECK(db.schema().at("events")[1].type == dates::type());
            auto one = [&](std::string_view sql) {
                auto rows = c.execute(sql).rows;
                CHECK(rows.size() == 1 && rows[0].size() == 1);
                return rows[0][0];
            };
            CHECK(one("SELECT CAST(DATE '2000-02-29' AS TEXT)") == Value(std::string("2000-02-29")));
            CHECK(one("SELECT CAST(NULL AS DATE)") == Value(Null(dates::type())));
            CHECK(one("SELECT CAST(CAST('1970-01-01' AS DATE) AS DATE)") == dates::value(0));
            CHECK(one("SELECT CAST(9223372036854775807 AS BIGINT)") == Value(INT64_MAX));
            CHECK(one("SELECT id FROM events WHERE happened = DATE '1970-01-01'") == Value(std::int64_t{1}));
            CHECK(
                one("SELECT id FROM events WHERE happened BETWEEN DATE '1969-12-30' AND DATE '1969-12-31'") ==
                Value(std::int64_t{2}));
            CHECK(one("SELECT count(*) FROM events WHERE happened IN (DATE '1970-01-01', NULL)") ==
                  Value(std::int64_t{1}));
            CHECK(one("SELECT count(*) FROM events WHERE happened IN (SELECT happened FROM events)") ==
                  Value(std::int64_t{2}));
            CHECK(one("SELECT DATE '1970-01-01' = NULL") == Value(Null(integer())));
            CHECK(one("SELECT DATE '1970-01-01' BETWEEN NULL AND DATE '1970-01-02'") ==
                  Value(Null(integer())));
            CHECK(one("SELECT CASE DATE '1970-01-01' WHEN DATE '1970-01-01' THEN 1 ELSE 0 END") ==
                  Value(std::int64_t{1}));
            CHECK(one("SELECT COALESCE(NULL,DATE '1970-01-01')") == dates::value(0));
            CHECK(one("SELECT CASE WHEN 0 THEN CAST('invalid' AS DATE) ELSE DATE '1970-01-01' END") ==
                  dates::value(0));
            CHECK(c.execute("SELECT happened,count(*) FROM events GROUP BY happened ORDER BY happened")
                      .rows.size() == 3);
            auto aggregates =
                c.execute("SELECT min(happened),max(happened),count(DISTINCT happened) FROM events").rows[0];
            CHECK(aggregates == Row({dates::value(-1), dates::value(0), std::int64_t{2}}));
            CHECK(c.execute("SELECT a.id,b.id FROM events a LEFT JOIN events b ON a.happened=b.happened "
                            "ORDER BY a.id")
                      .rows.size() == 3);
            auto original = c.execute("SELECT * FROM events ORDER BY id").rows;
            expect(ErrorCode::type, [&] {
                c.execute("INSERT INTO events(id,happened) VALUES(4,'2001-01-01'),(5,'1900-02-29')");
            });
            expect(ErrorCode::type, [&] {
                c.execute("UPDATE events SET happened=CASE WHEN id=1 THEN '2001-01-01' ELSE 'bad' END");
            });
            expect(ErrorCode::constraint,
                   [&] { c.execute("INSERT INTO events(id,happened) VALUES(4,'1970-01-01')"); });
            CHECK(c.execute("SELECT * FROM events ORDER BY id").rows == original);
            c.execute("BEGIN");
            c.execute("UPDATE events SET happened='2001-01-01' WHERE id=1");
            c.execute("ROLLBACK");
            CHECK(c.execute("SELECT * FROM events ORDER BY id").rows == original);
            c.execute("CREATE TABLE copied(d DATE)");
            c.execute("INSERT INTO copied SELECT CAST(happened AS TEXT) FROM events");
            CHECK(one("SELECT count(d) FROM copied") == Value(std::int64_t{2}));
            c.execute("UPDATE copied SET d='2000-02-29'");
            c.execute("ALTER TABLE copied ADD COLUMN added DATE DEFAULT DATE '0001-01-01'");
            c.execute("CREATE TABLE keyed(d DATE PRIMARY KEY)");
            c.execute("INSERT INTO keyed VALUES('2000-02-29')");
            CHECK(one("SELECT d FROM keyed WHERE d=DATE '2000-02-29'") == dates::parse("2000-02-29"));
            std::array<Value, 1> parameter{std::string("2024-02-29")};
            sql::Statement prepared("SELECT CAST(? AS DATE)");
            CHECK(c.execute(prepared, parameter).rows[0][0] == dates::parse("2024-02-29"));
            parameter[0] = std::string("2023-02-29");
            expect(ErrorCode::type, [&] { c.execute(prepared, parameter); });
            for (auto sql :
                 {"SELECT CAST(1 AS DATE) FROM events LIMIT 0", "SELECT happened+1 FROM events LIMIT 0",
                  "SELECT happened='1970-01-01' FROM events LIMIT 0",
                  "SELECT CAST(happened AS INTEGER) FROM events LIMIT 0",
                  "INSERT INTO copied(d) SELECT id FROM events LIMIT 0"})
                expect(ErrorCode::type, [&] { c.execute(sql); });
            expect(ErrorCode::type, [&] { c.execute("SELECT DATE '1900-02-29' FROM events LIMIT 0"); });
            c.execute("PRAGMA integrity_check");
            db.save(temp.path / "snapshot");
        }
        for (bool snapshot : {false, true}) {
            auto db =
                snapshot ? Database::load(temp.path / "snapshot", r) : Database::open(temp.path / "dates", r);
            sql::Connection c(db, r);
            CHECK(c.execute("SELECT happened FROM events WHERE id=1").rows[0][0] == dates::value(0));
            CHECK(c.execute("SELECT fallback FROM events WHERE id=3").rows[0][0] ==
                  dates::parse("2000-02-29"));
            CHECK(c.execute("SELECT d FROM keyed WHERE d=DATE '2000-02-29'").rows.size() == 1);
            c.execute("PRAGMA integrity_check");
            if (!snapshot)
                db.checkpoint();
        }
    });
}
