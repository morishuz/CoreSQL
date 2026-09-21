#include "check.hpp"
#include "coresql/sql.hpp"
#include <cmath>
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        Database db(registry);
        sql::Connection c(db, registry);
        c.execute("CREATE TABLE a(id INTEGER,x TEXT)");
        c.execute("CREATE TABLE b(id INTEGER,y TEXT)");
        c.execute("INSERT INTO a VALUES(1,'a'),(2,'b'),(NULL,'n')");
        c.execute("INSERT INTO b VALUES(2,'c'),(3,'d'),(NULL,'m')");
        auto result = c.execute("SELECT * FROM a JOIN b USING(id)");
        CHECK((result.columns == std::vector<std::string>{"id", "x", "y"}));
        CHECK((result.rows == std::vector<Row>{{std::int64_t{2}, std::string("b"), std::string("c")}}));
        CHECK(c.execute("SELECT a.*,b.* FROM a JOIN b USING(id)").rows[0].size() == 4);
        CHECK((c.execute("SELECT id FROM a RIGHT JOIN b USING(id) WHERE id IS NOT NULL ORDER BY id").rows ==
               std::vector<Row>{{std::int64_t{2}}, {std::int64_t{3}}}));
        CHECK((c.execute("SELECT id FROM a FULL JOIN b USING(id) WHERE id IS NOT NULL ORDER BY id").rows ==
               std::vector<Row>{{std::int64_t{1}}, {std::int64_t{2}}, {std::int64_t{3}}}));
        CHECK(c.execute("SELECT * FROM a LEFT JOIN b USING(id)").rows.size() == 3);
        CHECK((c.execute("SELECT id FROM a FULL JOIN b USING(id) JOIN b AS c USING(id) WHERE id IS NOT NULL "
                         "ORDER BY id")
                   .rows == std::vector<Row>{{std::int64_t{2}}, {std::int64_t{3}}}));
        CHECK(c.execute("SELECT id FROM (SELECT * FROM a JOIN b USING(id)) AS t").rows ==
              std::vector<Row>{{std::int64_t{2}}});
        c.execute("CREATE TABLE link(target INTEGER)");
        c.execute("INSERT INTO link VALUES(2)");
        CHECK((c.execute("SELECT id,x,b.y,target FROM a JOIN b USING(id) JOIN link ON id=target "
                         "RIGHT JOIN b AS last USING(id) WHERE id IS NOT NULL ORDER BY id")
                   .rows ==
               std::vector<Row>{{std::int64_t{2}, std::string("b"), std::string("c"), std::int64_t{2}},
                                {std::int64_t{3}, Null(text()), Null(text()), Null(integer())}}));
        CHECK(c.execute("SELECT id FROM a JOIN b USING(id) JOIN link ON "
                        "(SELECT id)=target RIGHT JOIN b AS last USING(id) WHERE x IS NOT NULL")
                  .rows == std::vector<Row>{{std::int64_t{2}}});
        CHECK(c.execute("SELECT id,(SELECT count(*) FROM b AS z WHERE z.id=a.id) FROM a JOIN b USING(id)")
                  .rows.size() == 1);
        expect(ErrorCode::schema, [&] { c.execute("SELECT * FROM a JOIN b USING(id,id)"); });
        expect(ErrorCode::schema, [&] { c.execute("SELECT * FROM a JOIN b USING(x)"); });
        expect(ErrorCode::schema, [&] { c.execute("SELECT id FROM a JOIN b USING(id),b AS c"); });
        c.execute("CREATE TABLE d(a INTEGER,b INTEGER, UNIQUE(a,b))");
        c.execute("INSERT INTO d VALUES(1,2),(1,3)");
        CHECK(c.execute("SELECT * FROM d JOIN d AS e USING(a,b)").rows.size() == 2);
        auto rounded =
            c.execute(
                 "SELECT round(2.5),round(-2.5),round(1.2345,2),round('3.14',1),round(NULL),round(1,NULL)")
                .rows[0];
        CHECK(std::get<double>(rounded[0]) == 3.0 && std::get<double>(rounded[1]) == -3.0);
        CHECK(std::get<double>(rounded[2]) == 1.23 && std::get<double>(rounded[3]) == 3.1);
        CHECK(is_null(rounded[4]) && is_null(rounded[5]));
        c.execute("CREATE TABLE r(id INTEGER PRIMARY KEY,n INTEGER CHECK(n>=0))");
        c.execute("INSERT INTO r VALUES(1,4),(2,5)");
        result = c.execute("UPDATE r SET id=id+10,n=n+1 RETURNING id,n AS updated");
        CHECK(result.changes == 2);
        CHECK((result.columns == std::vector<std::string>{"id", "updated"}));
        CHECK((result.rows ==
               std::vector<Row>{{std::int64_t{11}, std::int64_t{5}}, {std::int64_t{12}, std::int64_t{6}}}));
        c.execute("BEGIN");
        expect(ErrorCode::constraint, [&] { c.execute("UPDATE r SET n=-1 RETURNING *"); });
        CHECK(c.execute("SELECT sum(n) FROM r").rows == std::vector<Row>{{std::int64_t{11}}});
        expect(ErrorCode::schema, [&] { c.execute("DELETE FROM r RETURNING missing"); });
        CHECK(c.execute("SELECT count(*) FROM r").rows == std::vector<Row>{{std::int64_t{2}}});
        result = c.execute("DELETE FROM r WHERE id=11 RETURNING r.*");
        CHECK((result.rows == std::vector<Row>{{std::int64_t{11}, std::int64_t{5}}}));
        c.execute("ROLLBACK");
        CHECK(c.execute("DELETE FROM r WHERE id=99 RETURNING id").columns == std::vector<std::string>{"id"});
        CHECK(c.execute("DELETE FROM r RETURNING *").rows.size() == 2);
        db.begin().integrity_check();
    });
}
