#include "check.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry r;
        sql::install(r);
        Database db(r);
        sql::Connection c(db, r);
        c.execute("CREATE TABLE a(k INTEGER, name TEXT)");
        c.execute("CREATE TABLE b(k INTEGER, name TEXT)");
        c.execute("CREATE TABLE f(a INTEGER, b INTEGER, v INTEGER)");
        c.execute("INSERT INTO a VALUES(1,'FR'),(2,'DE'),(3,'GB'),(4,NULL),(1,'FR')");
        c.execute("INSERT INTO b VALUES(1,'FR'),(2,'DE'),(3,'GB'),(4,NULL)");
        c.execute("INSERT INTO f VALUES(1,2,1),(2,1,2),(1,1,3),(3,2,4),(4,1,5),(3,3,6)");
        const std::string prefix =
            "SELECT a.name,b.name,f.v FROM f,a,b WHERE f.v>0 AND f.a=a.k AND f.b=b.k AND ";
        // Explicit joins retain their order and provide an independent unpruned path.
        const std::string reference =
            "SELECT a.name,b.name,f.v FROM f JOIN a ON f.a=a.k JOIN b ON f.b=b.k WHERE f.v>0 AND ";
        for (const std::string predicate :
             {"((a.name='FR' AND b.name='DE') OR (a.name='DE' AND b.name='FR'))",
              "((a.name='FR' AND b.name='DE') OR b.name='GB')",
              "((a.name='FR' AND b.name='DE') OR (a.name IS NULL AND b.name='FR'))",
              "NOT ((a.name='FR' AND b.name='DE') OR (a.name='DE' AND b.name='FR'))"}) {
            auto suffix = predicate + " ORDER BY f.v,a.name,b.name";
            CHECK(c.execute(prefix + suffix).rows == c.execute(reference + suffix).rows);
        }
        auto result = c.execute(prefix + "((a.name='FR' AND b.name='DE') OR (a.name='DE' AND b.name='FR'))");
        CHECK(result.rows.size() == 3); // Duplicate join rows remain duplicates.
    });
}
