#include "check.hpp"
#include "coresql/sql.hpp"
#include "coresql/vector.hpp"
#include <array>

using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
        {
            auto db = Database::open(temp.path / "vectors", registry);
            sql::Connection c(db, registry);
            c.execute("CREATE TABLE embeddings(id INTEGER PRIMARY KEY, v VECTOR(2), loose VECTOR)");
            c.execute("INSERT INTO embeddings VALUES(1, '[1,2]', VECTOR '[3,4]'),(2,NULL,NULL)");
            auto result = c.execute(
                "SELECT vector_squared_l2(v, CAST('[4,6]' AS VECTOR(2))) FROM embeddings ORDER BY id");
            CHECK(std::get<double>(result.rows[0][0]) == 25);
            CHECK(is_null(result.rows[1][0]) && type_of(result.rows[1][0]) == real());
            CHECK(std::get<std::string>(
                      c.execute("SELECT CAST(v AS TEXT) FROM embeddings WHERE id=1").rows[0][0]) == "[1,2]");
            CHECK(vectors::elements(c.execute("SELECT vector(' [1e0, -2.5] ')").rows[0][0]) ==
                  (std::vector<float>{1, -2.5}));
            CHECK(vectors::elements(c.execute("SELECT CAST('[]' AS VECTOR(0))").rows[0][0]).empty());
            CHECK(is_null(c.execute("SELECT vector(NULL), vector_squared_l2(NULL,NULL)").rows[0][1]));
            CHECK(type_of(c.execute("SELECT CAST(NULL AS VECTOR(2))").rows[0][0]) == vectors::type(2));
            auto mixed = c.execute("SELECT CAST('[1,2]' AS VECTOR(2)) UNION ALL SELECT VECTOR '[3]'");
            CHECK(mixed.rows.size() == 2 && type_of(mixed.rows[0][0]) == vectors::type());
            CHECK(c.execute("WITH x AS (SELECT v FROM embeddings) SELECT v FROM x WHERE v IN (SELECT v FROM "
                            "embeddings)")
                      .rows.size() == 1);
            CHECK(vectors::elements(
                      c.execute("SELECT CASE WHEN 1 THEN VECTOR '[1]' ELSE CAST('bad' AS VECTOR) END")
                          .rows[0][0])
                      .size() == 1);
            for (auto input : {"[1,]", "[nan]", "[inf]", "[1e99]", "1,2", "[1 2]", "[1]x", "["}) {
                std::array<Value, 1> args{std::string(input)};
                expect(ErrorCode::type, [&] { c.execute("SELECT CAST(? AS VECTOR)", args); });
            }
            expect(ErrorCode::type, [&] { c.execute("SELECT CAST(12 AS VECTOR)"); });
            expect(ErrorCode::type, [&] { c.execute("SELECT CAST('[]' AS VECTOR(1,2))"); });
            expect(ErrorCode::type,
                   [&] { c.execute("SELECT vector_squared_l2(VECTOR '[1]',VECTOR '[1,2]')"); });
            expect(ErrorCode::type, [&] {
                c.execute("SELECT vector_squared_l2(CAST('[1]' AS VECTOR(1)),CAST('[1,2]' AS VECTOR(2)))");
            });
            expect(ErrorCode::type,
                   [&] { c.execute("INSERT INTO embeddings VALUES(3,'[1,2]',NULL),(4,'[1]',NULL)"); });
            CHECK(c.execute("SELECT id FROM embeddings").rows.size() == 2);
            c.execute("BEGIN");
            c.execute("SAVEPOINT edit");
            c.execute("UPDATE embeddings SET v=vector('[9,8]') WHERE id=1");
            c.execute("ROLLBACK TO edit");
            c.execute("RELEASE edit");
            c.execute("COMMIT");
            c.execute("BEGIN");
            c.execute("DELETE FROM embeddings");
            c.execute("ROLLBACK");
            CHECK(vectors::elements(c.execute("SELECT v FROM embeddings WHERE id=1").rows[0][0]) ==
                  (std::vector<float>{1, 2}));
            // Bound C++ values retain their bytes when SQL only changes dimensions.
            std::array<float, 2> elements{0.123456789f, -0.0f};
            auto original = vectors::value(elements);
            auto rebound =
                c.execute("SELECT CAST(? AS VECTOR(2))", std::array<Value, 1>{original}).rows[0][0];
            CHECK(std::ranges::equal(std::get<Opaque>(original).bytes(), std::get<Opaque>(rebound).bytes()));
            auto text_value = c.execute("SELECT CAST(? AS TEXT)", std::array<Value, 1>{original}).rows[0][0];
            auto roundtrip = c.execute("SELECT vector(?)", std::array<Value, 1>{text_value}).rows[0][0];
            CHECK(
                std::ranges::equal(std::get<Opaque>(original).bytes(), std::get<Opaque>(roundtrip).bytes()));
            CHECK(std::get<std::int64_t>(c.execute("SELECT VECTOR '[0]'=VECTOR '[-0]'").rows[0][0]) == 1);
            c.execute("BEGIN");
            c.execute("UPDATE embeddings SET loose=VECTOR '[5,6]' WHERE id=1");
            c.execute("COMMIT");
            db.checkpoint();
            // Reopen must also replay vector values written after the checkpoint.
            c.execute("UPDATE embeddings SET loose=VECTOR '[7]' WHERE id=1");
        }
        auto db = Database::open(temp.path / "vectors", registry);
        sql::Connection c(db, registry);
        CHECK(c.execute("SELECT v FROM embeddings").rows.size() == 2);
        CHECK(vectors::elements(c.execute("SELECT v FROM embeddings WHERE id=1").rows[0][0]) ==
              (std::vector<float>{1, 2}));
        CHECK(vectors::elements(c.execute("SELECT loose FROM embeddings WHERE id=1").rows[0][0]) ==
              (std::vector<float>{7}));
        db.begin().integrity_check();
    });
}
