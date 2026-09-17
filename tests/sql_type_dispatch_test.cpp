#include "check.hpp"
#include "../src/state.hpp"
#include "coresql/sql.hpp"
#include "coresql/timestamp.hpp"
#include <array>
#include <chrono>

using namespace coresql;
namespace {
sql::SqlTypeAdapter timestamp_adapter(bool repeatable = false) {
    sql::SqlTypeAdapter a;
    a.type_id = timestamps::type().id;
    a.names = {"timestamp"};
    a.declaration = [](const sql::SqlDeclaration& d) {
        if (!d.parameters.empty())
            throw Error(ErrorCode::type, "No timestamp parameters");
        return timestamps::type();
    };
    a.install = [](Registry& r) {
        timestamps::install(r);
        r.add(Function{"test.timestamp.year",
                       [](std::span<const Type> t) {
                           if (t.size() != 1 || t[0] != timestamps::type())
                               throw Error(ErrorCode::type, "Expected timestamp");
                           return integer();
                       },
                       [](std::span<const Value> v) -> Value {
                           using namespace std::chrono;
                           auto instant =
                               sys_time<microseconds>{microseconds{timestamps::microseconds(v[0])}};
                           return std::int64_t{int(year_month_day{floor<days>(instant)}.year())};
                       }});
    };
    a.can_convert = [](const Type&, const Type&) { return false; };
    a.convert = [](const Value&, const Type&, bool) -> Value {
        throw Error(ErrorCode::type, "No timestamp conversions");
    };
    a.repeatable = true;
    a.operation = [repeatable](std::string_view op,
                               std::span<const Type> t) -> std::optional<sql::SqlOperation> {
        if (op == "sql.extract.year" && t.size() == 1 && t[0] == timestamps::type())
            return sql::SqlOperation{"test.timestamp.year", {}, {}, repeatable};
        return {};
    };
    return a;
}
} // namespace
int main() {
    return tests([] {
        for (bool reversed : {false, true}) {
            sql::TypeAdapters types;
            types.add(reversed ? timestamp_adapter() : sql::date_adapter());
            types.add(reversed ? sql::date_adapter() : timestamp_adapter());
            Registry registry;
            sql::install(registry, types);
            Database db(registry);
            sql::Connection c(db, registry, types);
            c.execute("CREATE TABLE events(t TIMESTAMP, d DATE)");
            c.execute("INSERT INTO events VALUES(?, DATE '2024-03-01')",
                      std::array<Value, 1>{timestamps::value(0)});
            CHECK(c.execute("SELECT EXTRACT(YEAR FROM t),EXTRACT(YEAR FROM d) FROM events").rows ==
                  (std::vector<Row>{{std::int64_t{1970}, std::int64_t{2024}}}));
            CHECK(is_null(c.execute("SELECT EXTRACT(YEAR FROM NULL)").rows[0][0]));
            CHECK(is_null(c.execute("SELECT EXTRACT(YEAR FROM CAST(NULL AS TIMESTAMP))").rows[0][0]));
            CHECK(is_null(c.execute("SELECT EXTRACT(YEAR FROM CAST(NULL AS DATE))").rows[0][0]));
            CHECK(!types.operation("sql.extract.year", std::array{real()}));
            expect(ErrorCode::unsupported, [&] { c.execute("SELECT EXTRACT(YEAR FROM 1.5)"); });
            expect(ErrorCode::unsupported,
                   [&] { c.execute("SELECT EXTRACT(YEAR FROM CAST(NULL AS REAL))"); });
        }
        // Repeatable adapter operations stream aggregate input; nonrepeatable
        // operations must retain the conservative execution path.
        for (bool repeatable : {false, true}) {
            sql::TypeAdapters types;
            types.add(sql::date_adapter());
            types.add(timestamp_adapter(repeatable));
            Registry r;
            sql::install(r, types);
            Database data(r);
            sql::Connection c(data, r, types);
            c.execute("CREATE TABLE events(t TIMESTAMP, d DATE)");
            c.execute("BEGIN");
            for (int i = 0; i < 600; ++i)
                c.execute("INSERT INTO events VALUES(?, DATE '2024-03-01')",
                          std::array<Value, 1>{timestamps::value(0)});
            c.execute("COMMIT");
            detail::retained_matches() = 0;
            CHECK(c.execute("SELECT EXTRACT(YEAR FROM d),count(*) FROM events GROUP BY EXTRACT(YEAR FROM d)")
                      .rows == (std::vector<Row>{{std::int64_t{2024}, std::int64_t{600}}}));
            CHECK(detail::retained_matches() < 10);
            detail::retained_matches() = 0;
            CHECK(c.execute("SELECT EXTRACT(YEAR FROM t),count(*) FROM events GROUP BY EXTRACT(YEAR FROM t)")
                      .rows == (std::vector<Row>{{std::int64_t{1970}, std::int64_t{600}}}));
            CHECK(repeatable ? detail::retained_matches() < 10 : detail::retained_matches() >= 600);
            c.execute("CREATE TABLE keys(k INTEGER)");
            c.execute("INSERT INTO keys VALUES(1970),(1970)");
            detail::QueryCounters counters;
            {
                detail::QueryCounterScope scope(counters);
                CHECK(c.execute("SELECT (SELECT count(*) FROM events e WHERE EXTRACT(YEAR FROM e.t)="
                                "keys.k) FROM keys")
                          .rows == (std::vector<Row>{{std::int64_t{600}}, {std::int64_t{600}}}));
            }
            CHECK(repeatable ? counters.subquery_cache_hits >= 1 : counters.subquery_cache_hits == 0);
        }
        // Shared scalar expression policy does not belong to the INTEGER adapter.
        sql::TypeAdapters real_only(false);
        real_only.add(sql::real_adapter());
        Registry registry;
        sql::install(registry, real_only);
        Database db(registry);
        sql::Connection c(db, registry, real_only);
        CHECK(c.execute("SELECT 1.5+2.5,abs(-2.5)").rows == (std::vector<Row>{{4.0, 2.5}}));
        CHECK(!real_only.find(integer()));
        CHECK(c.execute("SELECT CASE WHEN 1 THEN 1.5 ELSE 'text' END").rows[0][0] == Value(1.5));
    });
}
