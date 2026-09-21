#include "check.hpp"
#include "coresql/sql.hpp"
#include <array>
using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
        {
            auto db = Database::open(temp.path / "db", registry);
            sql::Connection c(db, registry);
            sql::Statement p("SELECT :Id,:Id,@Id,$id,?7,?");
            CHECK(p.parameter_count() == 8);
            CHECK(p.parameter_index(":Id") == 1 && p.parameter_index("@Id") == 2 &&
                  p.parameter_index("$id") == 3);
            CHECK(!p.parameter_index(":id") && !p.parameter_index("none"));
            auto rows = c.execute(p, std::array<Value, 8>{std::int64_t{1}, std::int64_t{2}, std::int64_t{3},
                                                          std::int64_t{4}, std::int64_t{5}, std::int64_t{6},
                                                          std::int64_t{7}, std::int64_t{8}})
                            .rows;
            CHECK((rows == std::vector<Row>{{std::int64_t{1}, std::int64_t{1}, std::int64_t{2},
                                             std::int64_t{3}, std::int64_t{7}, std::int64_t{8}}}));
            c.execute("CREATE TABLE frames(session INTEGER, frame INTEGER, label TEXT, CONSTRAINT identity "
                      "PRIMARY KEY(session,frame), UNIQUE(label,frame))");
            c.execute("INSERT INTO frames VALUES(1,1,'a'),(1,2,'b'),(2,1,'c')");
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(1,1,'d')"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(NULL,3,'d')"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(5,1,'c')"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(3,3,'d'),(1,1,'e')"); });
            CHECK(c.execute("SELECT * FROM frames").rows.size() == 3);
            {
                auto tx = db.begin();
                expect(ErrorCode::constraint, [&] {
                    tx.insert("frames", {std::int64_t{1}, std::int64_t{1}, std::string("native")});
                });
                auto savepoint = tx.savepoint();
                tx.insert("frames", {std::int64_t{4}, std::int64_t{4}, Null(text())});
                savepoint.rollback();
                CHECK(tx.query(Query{"frames"}).rows.size() == 3);
            }
            c.execute("CREATE TABLE nullable_keys(a INTEGER,b TEXT,UNIQUE(a,b))");
            c.execute("INSERT INTO nullable_keys VALUES(1,NULL),(1,NULL),(NULL,'x'),(NULL,'x')");
            CHECK(c.execute("SELECT * FROM nullable_keys").rows.size() == 4);
            auto result = c.execute("SELECT f.*,f.session+1 AS next FROM frames f ORDER BY 1,2");
            CHECK((result.columns == std::vector<std::string>{"session", "frame", "label", "next"}));
            CHECK(result.rows.size() == 3 && result.rows[0].size() == 4);
            CHECK(c.execute("WITH x AS (SELECT f.* FROM frames f) SELECT x.* FROM x").rows.size() == 3);
            expect(ErrorCode::schema, [&] { c.execute("SELECT missing.* FROM frames LIMIT 0"); });
            expect(ErrorCode::schema, [&] { c.execute("SELECT f.* AS invalid FROM frames f"); });
            expect(ErrorCode::unsupported, [&] { c.execute("SELECT count(frames.*) FROM frames"); });
            c.execute("CREATE TABLE ids(id INTEGER, value TEXT, PRIMARY KEY(id))");
            c.execute("INSERT INTO ids(value) VALUES('generated')");
            CHECK(std::get<std::int64_t>(c.execute("SELECT id FROM ids").rows[0][0]) == 1);
            expect(ErrorCode::schema, [&] {
                c.execute("CREATE TABLE invalid(a INTEGER PRIMARY KEY,b INTEGER,PRIMARY KEY(b))");
            });
            CHECK(!db.schema().contains("invalid"));
            c.execute("CREATE TABLE quoted(\"table.0\" TEXT UNIQUE, a INTEGER, b INTEGER, PRIMARY KEY(a,b))");
            c.execute("INSERT INTO quoted VALUES('label',1,2)");
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO quoted VALUES('other',1,2)"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO quoted VALUES('label',2,3)"); });
            db.checkpoint();
        }
        auto db = Database::open(temp.path / "db", registry);
        sql::Connection c(db, registry);
        expect(ErrorCode::constraint, [&] { c.execute("UPDATE frames SET session=1 WHERE session=2"); });
        CHECK(c.execute("SELECT * FROM frames").rows.size() == 3);
        expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(NULL,4,'e')"); });
        db.begin().integrity_check();
    });
}
