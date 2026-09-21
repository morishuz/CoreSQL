#include "check.hpp"
#include "coresql/sql.hpp"
#include <sqlite3.h>
#include <random>
using namespace coresql;
struct Reference {
    sqlite3* db = nullptr;
    Reference() { CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK); }
    ~Reference() { sqlite3_close(db); }
    std::vector<Row> run(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> cleanup(statement, sqlite3_finalize);
        std::vector<Row> result;
        int rc;
        while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
            Row row;
            for (int i = 0; i < sqlite3_column_count(statement); ++i) {
                switch (sqlite3_column_type(statement, i)) {
                case SQLITE_INTEGER:
                    row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(statement, i)));
                    break;
                case SQLITE_FLOAT:
                    row.emplace_back(sqlite3_column_double(statement, i));
                    break;
                case SQLITE_NULL:
                    row.emplace_back(Null(integer()));
                    break;
                case SQLITE_TEXT:
                    row.emplace_back(
                        std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, i)),
                                    static_cast<std::size_t>(sqlite3_column_bytes(statement, i))));
                    break;
                default:
                    throw std::runtime_error("Unexpected reference type");
                }
            }
            result.push_back(std::move(row));
        }
        if (rc != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
        return result;
    }
};
int main() {
    return tests([] {
        Registry r;
        sql::install(r);
        Database db(r);
        sql::Connection core(db, r);
        Reference reference;
        auto compare = [&](const std::string& text) {
            auto a = core.execute(text).rows, b = reference.run(text);
            bool same = a.size() == b.size();
            for (std::size_t i = 0; same && i < a.size(); ++i) {
                same = a[i].size() == b[i].size();
                for (std::size_t j = 0; same && j < a[i].size(); ++j)
                    same = (is_null(a[i][j]) && is_null(b[i][j])) || a[i][j] == b[i][j];
            }
            if (!same)
                throw std::runtime_error("SQL result differs: " + text);
        };
        compare("CREATE TABLE upserted(id INTEGER PRIMARY KEY,label TEXT UNIQUE,n INTEGER CHECK(n>=0))");
        compare("INSERT INTO upserted VALUES(1,'a',2),(2,'b',3)");
        compare("INSERT INTO upserted VALUES(1,'other',5) ON CONFLICT(id) DO UPDATE SET n=n+excluded.n "
                "RETURNING *");
        compare("INSERT INTO upserted VALUES(3,'a',4) ON CONFLICT(label) DO NOTHING RETURNING id");
        compare("INSERT INTO upserted VALUES(3,'a',4) ON CONFLICT(label) DO UPDATE SET n=excluded.n WHERE "
                "n>100 RETURNING *");
        compare("INSERT INTO upserted(label,n) VALUES('a',20),('c',30) ON CONFLICT(label) DO UPDATE SET "
                "n=excluded.n RETURNING *");
        compare("SELECT * FROM upserted ORDER BY id");
        compare("SELECT round(2.5),round(-2.5),round(1.2345,2),round('3.14',1),round(NULL),round(1,NULL)");
        compare("UPDATE upserted SET n=n+1 WHERE id=1 RETURNING id,n AS updated");
        compare("DELETE FROM upserted WHERE id=2 RETURNING *");
        compare("CREATE TABLE ua(id INTEGER,x TEXT)");
        compare("CREATE TABLE ub(id INTEGER,y TEXT)");
        compare("INSERT INTO ua VALUES(1,'a'),(2,'b'),(NULL,'n')");
        compare("INSERT INTO ub VALUES(2,'c'),(3,'d'),(NULL,'m')");
        compare("SELECT * FROM ua JOIN ub USING(id) ORDER BY id");
        compare("SELECT * FROM ua LEFT JOIN ub USING(id) ORDER BY id");
        compare("SELECT * FROM ua RIGHT JOIN ub USING(id) ORDER BY id");
        compare("SELECT * FROM ua FULL JOIN ub USING(id) WHERE id IS NOT NULL ORDER BY id");
        compare("SELECT ua.*,ub.* FROM ua JOIN ub USING(id) ORDER BY id");
        compare("CREATE TABLE ul(target INTEGER)");
        compare("INSERT INTO ul VALUES(2)");
        compare(
            "SELECT id,x,ub.y,target FROM ua JOIN ub USING(id) JOIN ul ON ua.id=target RIGHT JOIN ub AS last "
            "USING(id) WHERE id IS NOT NULL ORDER BY id");
        compare("SELECT id FROM ua JOIN ub USING(id) JOIN ul ON (SELECT ua.id)=target RIGHT JOIN ub AS last "
                "USING(id) WHERE x IS NOT NULL");
        compare("CREATE TABLE t(id INTEGER PRIMARY KEY,v INTEGER,label TEXT)");
        compare("BEGIN");
        for (int i = 0; i < 80; ++i)
            compare("INSERT INTO t VALUES(" + std::to_string(i) + "," + std::to_string(i % 13) + ",'" +
                    (i % 2 ? "alpha" : "beta") + "')");
        compare("COMMIT");
        for (const auto& predicate :
             {"v NOT BETWEEN 3 AND 7", "NOT v BETWEEN 3 AND 7", "v NOT BETWEEN NULL AND 7",
              "v NOT BETWEEN 3 AND NULL", "v NOT BETWEEN 7 AND 3 OR id=1"})
            compare("SELECT id,v FROM t WHERE " + std::string(predicate) + " ORDER BY id");
        compare("SELECT 2 NOT BETWEEN 3 AND 7, 5 NOT BETWEEN 3 AND 7, NULL NOT BETWEEN 3 AND 7, 2 NOT "
                "BETWEEN NULL AND 1");
        for (const auto& keys : {"v DESC,label,id DESC", "label,v DESC,id", "2 DESC,3,1 DESC"})
            for (const auto& limit : {"", " LIMIT 0", " LIMIT 1", " LIMIT 17"})
                compare("SELECT id,v,label FROM t ORDER BY " + std::string(keys) + limit);
        compare("SELECT id AS x,v AS y FROM t ORDER BY y DESC,x");
        compare("SELECT DISTINCT v,label FROM t ORDER BY v DESC,label LIMIT 8");
        compare("SELECT id,(SELECT x.id FROM t AS x WHERE x.v=t.v ORDER BY x.v,x.id DESC LIMIT 1) FROM t "
                "ORDER BY v,id");
        compare("SELECT CASE WHEN NULL THEN 1 WHEN 0 THEN 2 ELSE 3 END,CASE NULL WHEN NULL THEN 1 END");
        compare("SELECT id,CASE v WHEN 0 THEN 9 WHEN 1 THEN 8 ELSE 7 END FROM t ORDER BY 2,id DESC");
        compare("SELECT id,CASE WHEN v<3 THEN label ELSE NULL END FROM t ORDER BY 2,id");
        compare("SELECT id FROM t WHERE CASE WHEN v<3 THEN 1 ELSE 0 END ORDER BY v,id");
        compare(
            "SELECT id,EXISTS(SELECT x.id,x.v FROM t AS x WHERE x.v=t.v AND x.id<t.id) FROM t ORDER BY id");
        compare("SELECT id FROM t WHERE NOT EXISTS(SELECT NULL FROM t AS x WHERE x.v=t.v AND x.id<t.id) "
                "ORDER BY id");
        compare("SELECT EXISTS(SELECT id FROM t LIMIT 0),EXISTS(SELECT count(*) FROM t WHERE "
                "id<0),EXISTS(SELECT id FROM t WHERE id<0) FROM t LIMIT 1");
        compare("SELECT id FROM t WHERE EXISTS(SELECT x.id FROM t AS x WHERE x.id=t.id AND EXISTS(SELECT "
                "y.id FROM t AS y WHERE y.id=x.id)) ORDER BY id");
        compare("CREATE TABLE nullable(a INTEGER,b TEXT)");
        compare("INSERT INTO nullable VALUES(NULL,'x'),(2,NULL),(NULL,NULL)");
        compare("SELECT coalesce(a,8),b IS NULL FROM nullable ORDER BY 1,2");
        compare("UPDATE nullable SET b=NULL WHERE a=2");
        compare("SELECT a,b FROM nullable ORDER BY a,b");
        compare("CREATE TABLE unqualified(k INTEGER,other INTEGER)");
        compare("INSERT INTO unqualified VALUES(1,2),(2,3)");
        compare("SELECT id,other FROM t,unqualified WHERE id=k ORDER BY id");
        std::mt19937 random(617);
        std::function<std::string(int)> condition = [&](int depth) {
            if (!depth || random() % 3 == 0) {
                static const char* ops[] = {"=", "<>", "<", ">", ">=", "<="};
                return "(v" + std::string(ops[random() % 6]) + std::to_string(random() % 16) + ")";
            }
            auto a = condition(depth - 1);
            if (random() % 3 == 0)
                return "(NOT " + a + ")";
            return "(" + a + (random() % 2 ? " AND " : " OR ") + condition(depth - 1) + ")";
        };
        for (int i = 0; i < 200; ++i) {
            auto where = condition(3);
            compare("SELECT id,(v+2)*3,v&1 FROM t WHERE " + where + " ORDER BY id " +
                    (i % 2 ? "DESC" : "ASC") + " LIMIT " + std::to_string(i % 81));
            compare("SELECT count(*),sum(v),min(v),max(v) FROM t WHERE " + where);
            compare("BEGIN");
            compare("UPDATE t SET v=v+1 WHERE " + where);
            compare("SELECT * FROM t ORDER BY id");
            compare("ROLLBACK");
        }
        for (const auto* text :
             {"SELECT 1+2*3,8&3+1,8/3,8%3,'a'||'b'", "SELECT NULL AND 0,NULL OR 1,NOT NULL,1/0",
              "SELECT id AS v FROM t ORDER BY v DESC LIMIT 3", "SELECT rowid FROM t ORDER BY id",
              "SELECT like('A%','alpha'),-1.25,-9223372036854775808",
              "SELECT label FROM t WHERE label LIKE '%PH%' ORDER BY id", "SELECT sum(v) FROM t WHERE id<0",
              "SELECT id,(SELECT v FROM t AS inside WHERE inside.id=t.id) FROM t ORDER BY id",
              "SELECT a.id,b.v FROM t AS a,t AS b WHERE b.id BETWEEN 2 AND 7 AND a.id=b.id ORDER BY a.id"})
            compare(text);
        compare("CREATE TABLE quoted(\"null\" INTEGER,\"select\" TEXT)");
        compare("INSERT INTO quoted VALUES(1,';--')");
        compare("SELECT \"null\",\"select\" FROM quoted");
    });
}
