#include "check.hpp"
#include "coresql/sql.hpp"
#include <array>

using namespace coresql;
int main() {
    return tests([] {
        // Explicit composition exercises the same interface as optional domain types.
        sql::TypeAdapters types(false);
        CHECK(!types.named("integer"));
        types.add(sql::integer_adapter());
        types.add(sql::real_adapter());
        types.add(sql::text_adapter());
        CHECK(types.named("BIGINT") == types.find(integer()));
        CHECK(types.named("varchar") == types.find(text()));
        CHECK(types.convert(std::string("12tail"), integer(), true) == Value(std::int64_t{12}));
        expect(ErrorCode::type, [&] { types.convert(std::string("12tail"), integer(), false); });
        CHECK(types.find(integer())->apply_affinity(std::string("12tail")) == Value(std::string("12tail")));
        CHECK(types.find(real())->apply_affinity(std::string("12")) == Value(std::int64_t{12}));
        Registry registry;
        sql::install(registry, types);
        TempDirectory temp;
        {
            auto db = Database::open(temp.path / "scalars", registry);
            sql::Connection c(db, registry, types);
            c.execute(
                "CREATE TABLE values_table(id INT PRIMARY KEY, i BIGINT, r REAL, t VARCHAR(4294967296), d)");
            c.execute("INSERT INTO values_table VALUES(1,'12',3,'4',5),(2,NULL,NULL,NULL,'6')");
            auto row = c.execute("SELECT i,r,t,d FROM values_table WHERE id=1").rows[0];
            CHECK(row == (Row{std::int64_t{12}, 3.0, std::string("4"), std::int64_t{5}}));
            auto casts = c.execute("SELECT CAST('12tail' AS INTEGER),CAST('1.5tail' AS REAL),CAST(2.0 AS "
                                   "VARCHAR),CAST('bad' AS REAL)")
                             .rows[0];
            CHECK(casts == (Row{std::int64_t{12}, 1.5, std::string("2.0"), 0.0}));
            auto arithmetic =
                c.execute("SELECT 7/2,7/2.0,'7'+2,'7.5'+2,'bad'+2,'3.8'%2,abs('-2'),-CAST('3' AS NUMERIC)")
                    .rows[0];
            CHECK(arithmetic == (Row{std::int64_t{3}, 3.5, std::int64_t{9}, 9.5, std::int64_t{2}, 1.0, 2.0,
                                     std::int64_t{-3}}));
            CHECK(c.execute("SELECT id FROM values_table WHERE i='12'").rows.size() == 1);
            CHECK(c.execute("SELECT id FROM values_table WHERE t=4").rows.size() == 1);
            CHECK(std::get<std::int64_t>(c.execute("SELECT CAST(1 AS INTEGER)='1'").rows[0][0]) == 1);
            CHECK(std::get<std::int64_t>(c.execute("SELECT CAST(1 AS TEXT)=1").rows[0][0]) == 1);
            CHECK(c.execute("SELECT sum(d) FROM values_table").rows[0][0] == Value(std::int64_t{11}));
            auto mixed =
                c.execute("SELECT CASE WHEN id=1 THEN i ELSE 'text' END FROM values_table ORDER BY id");
            CHECK(mixed.rows[0][0] == Value(std::int64_t{12}) &&
                  mixed.rows[1][0] == Value(std::string("text")));
            CHECK(c.execute("SELECT i FROM values_table UNION ALL SELECT t FROM values_table").rows.size() ==
                  4);
            auto nulls = c.execute("SELECT NULL+1,1/0,CAST(NULL AS TEXT),CAST(NULL AS REAL)").rows[0];
            CHECK(std::all_of(nulls.begin(), nulls.end(), is_null));
            CHECK(type_of(nulls[2]) == text() && type_of(nulls[3]) == real());
            expect(ErrorCode::type,
                   [&] { c.execute("INSERT INTO values_table VALUES(3,3,3,'3',3),(4,'3x',4,'4',4)"); });
            CHECK(c.execute("SELECT id FROM values_table").rows.size() == 2);
            c.execute("BEGIN");
            c.execute("UPDATE values_table SET i='13' WHERE id=1");
            c.execute("ROLLBACK");
            CHECK(c.execute("SELECT i FROM values_table WHERE id=1").rows[0][0] == Value(std::int64_t{12}));
            c.execute("CREATE TABLE aliases(a INTEGER,b TEXT,c VARCHAR)");
            expect(ErrorCode::unsupported, [&] { c.execute("CREATE TABLE bad(a VARCHAR(0))"); });
            expect(ErrorCode::unsupported, [&] { c.execute("SELECT CAST(1 AS VARCHAR(2))"); });
            db.checkpoint();
        }
        auto db = Database::open(temp.path / "scalars", registry);
        sql::Connection c(db, registry, types);
        CHECK(c.execute("SELECT d+1 FROM values_table ORDER BY id").rows ==
              (std::vector<Row>{{std::int64_t{6}}, {std::int64_t{7}}}));
        db.begin().integrity_check();
        // Native scalar policy is supplied through the interface, including
        // affinity used by both binary and membership comparisons.
        sql::TypeAdapters customized(false);
        auto integers = sql::integer_adapter();
        integers.names.push_back("whole");
        auto base_affinity = integers.apply_affinity;
        integers.apply_affinity = [base_affinity](const Value& value) {
            if (auto text = std::get_if<std::string>(&value); text && *text == "twelve")
                return Value(std::int64_t{12});
            return base_affinity(value);
        };
        customized.add(integers);
        auto reals = sql::real_adapter();
        auto real_affinity = reals.apply_affinity;
        reals.apply_affinity = [real_affinity](const Value& value) {
            if (auto text = std::get_if<std::string>(&value); text && *text == "real-twelve")
                return Value(12.0);
            return real_affinity(value);
        };
        customized.add(reals);
        customized.add(sql::text_adapter());
        Registry custom_registry;
        sql::install(custom_registry, customized);
        Database custom_db(custom_registry);
        sql::Connection custom(custom_db, custom_registry, customized);
        custom.execute("CREATE TABLE custom(i WHOLE)");
        custom.execute("INSERT INTO custom VALUES(12)");
        CHECK(custom.execute("SELECT i FROM custom WHERE i='twelve'").rows.size() == 1);
        CHECK(custom.execute("SELECT i FROM custom WHERE i IN ('twelve')").rows.size() == 1);
        CHECK(custom.execute("SELECT i FROM custom WHERE i BETWEEN 'twelve' AND 'twelve'").rows.size() == 1);
        custom.execute("CREATE TABLE real_values(r REAL)");
        custom.execute("INSERT INTO real_values VALUES(12),(NULL)");
        for (const auto& predicate :
             {"r='real-twelve'", "'real-twelve'=r", "r BETWEEN 'real-twelve' AND 'real-twelve'",
              "r IN ('real-twelve')", "r IN (SELECT 'real-twelve')",
              "CASE r WHEN 'real-twelve' THEN 1 ELSE 0 END", "CAST(r AS REAL)='real-twelve'",
              "r='real-twelve' AND r IS NOT NULL"})
            if (custom.execute(std::string("SELECT r FROM real_values WHERE ") + predicate).rows.size() != 1)
                throw std::runtime_error(std::string("Wrong affinity result for ") + predicate);
        CHECK(custom.execute("SELECT 'real-twelve' IN (SELECT r FROM real_values)").rows[0][0] ==
              Value(std::int64_t{1}));
        CHECK(custom.execute("SELECT r FROM real_values WHERE r='twelve'").rows.empty());
        CHECK(custom.execute("SELECT i FROM custom WHERE i='real-twelve'").rows.empty());
        CHECK(is_null(custom.execute("SELECT r='real-twelve' FROM real_values WHERE r IS NULL").rows[0][0]));
        CHECK(registry.function("sql.numeric_add").infer(std::array{integer(), real()}) == real());
        CHECK(registry.function("sql.numeric_add").infer(std::array{integer(), integer()}) == integer());
        CHECK(registry.function("sql.numeric_add").infer(std::array{integer(), text()}).id == "sql.value");
    });
}
