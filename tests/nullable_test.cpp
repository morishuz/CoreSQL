#include "check.hpp"
#include "coresql/sql.hpp"
#include "coresql/timestamp.hpp"
using namespace coresql;
int main(int argc,char** argv){return tests([&]{
    CHECK(argc==2);TempDirectory temp;
    Registry r;timestamps::install(r);sql::install(r);
    {
        auto db=Database::open(temp.path/"db",r);sql::Connection c(db,r);
        c.execute("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER UNIQUE, label VARCHAR(40), required INTEGER NOT NULL DEFAULT 4)");
        c.execute("INSERT INTO t(id,v) VALUES(1,NULL),(2,NULL),(3,9)");
        CHECK(c.execute("SELECT count(*),count(v),sum(v) FROM t").rows[0]==Row({std::int64_t{3},std::int64_t{1},std::int64_t{9}}));
        CHECK(c.execute("SELECT id FROM t WHERE v IS NULL ORDER BY id").rows.size()==2);
        CHECK(c.execute("SELECT id FROM t WHERE v IS NOT NULL").rows[0][0]==Value(std::int64_t{3}));
        c.execute("BEGIN");c.execute("UPDATE t SET v=NULL WHERE id=3");c.execute("ROLLBACK");
        expect(ErrorCode::constraint,[&]{c.execute("UPDATE t SET required=NULL");});
        expect(ErrorCode::constraint,[&]{c.execute("INSERT INTO t VALUES(4,9,NULL,4)");});
        c.execute("REPLACE INTO t(id,v) VALUES(4,NULL)");CHECK(c.execute("SELECT count(*) FROM t").rows[0][0]==Value(std::int64_t{4}));
        c.execute("CREATE INDEX by_v ON t(v)");
        CHECK(c.execute("SELECT id FROM t WHERE v BETWEEN 8 AND 10").rows.size()==1);
        c.execute("UPDATE t SET v=8 WHERE id=1");c.execute("UPDATE t SET v=NULL WHERE id=1");
        c.execute("ALTER TABLE t ADD COLUMN extra TEXT");c.execute("PRAGMA integrity_check");
        auto tx=db.begin();Column custom{"stamp",timestamps::type()};custom.nullable=true;
        tx.create_table("custom",{custom});tx.insert("custom",{Null(timestamps::type())});tx.commit();
        db.save(temp.path/"snapshot");
    }
    for(bool snapshot:{false,true}) {
        auto db=snapshot?Database::load(temp.path/"snapshot",r):Database::open(temp.path/"db",r);sql::Connection c(db,r);
        CHECK(db.schema().at("t")[1].nullable);CHECK(!db.schema().at("t")[0].nullable);
        CHECK(c.execute("SELECT count(*) FROM t WHERE v IS NULL").rows[0][0]==Value(std::int64_t{3}));
        CHECK(is_null(db.query(Query{"custom",{},{},{}}).rows[0][0]));
        c.execute("PRAGMA integrity_check");if(!snapshot)db.checkpoint();
    }
    {
        auto db=Database::load(std::filesystem::path(argv[1]).parent_path()/"before-nullable.snapshot",r);
        CHECK(!db.schema().at("legacy")[1].nullable);
        CHECK(db.query(Query{"legacy",{column("label")},{},{}}).rows[0][0]==Value(std::string("old")));
    }
    std::filesystem::copy_file(argv[1],temp.path/"legacy");
    {
        auto db=Database::open(temp.path/"legacy",r);CHECK(!db.schema().at("legacy")[1].nullable);
        sql::Connection c(db,r);CHECK(c.execute("SELECT label FROM legacy").rows[0][0]==Value(std::string("old")));
        c.execute("INSERT INTO legacy VALUES(8,'new')");
    }
    {
        auto db=Database::open(temp.path/"legacy",r);sql::Connection c(db,r);
        CHECK(c.execute("SELECT count(*) FROM legacy").rows[0][0]==Value(std::int64_t{2}));
        c.execute("PRAGMA integrity_check");
    }
});}
