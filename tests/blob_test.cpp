#include "check.hpp"
#include "coresql/blob.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
        {
            auto db = Database::open(temp.path / "db", registry);
            sql::Connection c(db, registry);
            c.execute(
                "CREATE TABLE payloads(id INTEGER PRIMARY KEY,data BLOB UNIQUE,CHECK(length(data)<=4))");
            c.execute("INSERT INTO payloads VALUES(1,X'00ff'),(2,X''),(3,NULL)");
            CHECK((c.execute("SELECT hex(data),length(data) FROM payloads ORDER BY id").rows[0] ==
                   Row{std::string("00FF"), std::int64_t{2}}));
            CHECK(c.execute("SELECT data FROM payloads WHERE id=1").rows[0][0] == blobs::from_hex("00FF"));
            CHECK(c.execute("SELECT CAST(CAST(? AS BLOB) AS TEXT)", Row{std::string("a\0b", 3)}).rows[0][0] ==
                  Value(std::string("a\0b", 3)));
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO payloads VALUES(4,X'00FF')"); });
            expect(ErrorCode::type, [&] { c.execute("INSERT INTO payloads VALUES(4,'text')"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO payloads VALUES(4,X'0001020304')"); });
            expect(ErrorCode::type, [&] { sql::Statement("SELECT X'0'"); });
            expect(ErrorCode::type, [&] { sql::Statement("SELECT X'gg'"); });
            CHECK(c.execute("SELECT id FROM payloads WHERE data=X'00ff'").rows ==
                  std::vector<Row>{{std::int64_t{1}}});
            CHECK(c.execute("SELECT count(DISTINCT data) FROM payloads").rows ==
                  std::vector<Row>{{std::int64_t{2}}});
            c.execute("BEGIN");
            c.execute("UPDATE payloads SET data=X'80' WHERE id=1");
            c.execute("ROLLBACK");
            db.checkpoint();
        }
        auto db = Database::open(temp.path / "db", registry);
        sql::Connection c(db, registry);
        CHECK(c.execute("SELECT data FROM payloads WHERE id=1").rows[0][0] == blobs::from_hex("00ff"));
        db.begin().integrity_check();
    });
}
