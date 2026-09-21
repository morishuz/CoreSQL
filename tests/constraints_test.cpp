#include "check.hpp"
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
            c.execute("CREATE TABLE models(id INTEGER PRIMARY KEY, label TEXT)");
            c.execute("INSERT INTO models VALUES(1,'model')");
            c.execute("CREATE TABLE frames(id INTEGER PRIMARY KEY,model INTEGER REFERENCES models(id),"
                      "seen INTEGER CHECK(seen>=0),CONSTRAINT positive CHECK(id>0))");
            c.execute("INSERT INTO frames VALUES(1,1,0),(2,NULL,NULL)");
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(3,99,0)"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(3,1,-1)"); });
            expect(ErrorCode::constraint, [&] { c.execute("UPDATE frames SET seen=-1"); });
            expect(ErrorCode::constraint, [&] { c.execute("DELETE FROM models"); });
            expect(ErrorCode::constraint, [&] { c.execute("UPDATE models SET id=2"); });
            expect(ErrorCode::constraint, [&] { c.execute("DROP TABLE models"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(3,1,0),(4,99,0)"); });
            CHECK(c.execute("SELECT count(*) FROM frames").rows == std::vector<Row>{{std::int64_t{2}}});
            auto tx = db.begin();
            expect(ErrorCode::constraint,
                   [&] { tx.insert("frames", {std::int64_t{3}, std::int64_t{1}, std::int64_t{-1}}); });
            auto point = tx.savepoint();
            tx.set_constraints("frames", {});
            tx.insert("frames", {std::int64_t{-1}, std::int64_t{99}, std::int64_t{-1}});
            point.rollback();
            CHECK(tx.constraints("frames").checks.size() == 2);
            tx.rollback();
            c.execute("CREATE TABLE composite(a INTEGER,b TEXT,PRIMARY KEY(a,b))");
            c.execute("INSERT INTO composite VALUES(1,'a')");
            c.execute("CREATE TABLE child(a INTEGER,b TEXT,FOREIGN KEY(a,b) REFERENCES composite(a,b))");
            c.execute("INSERT INTO child VALUES(1,'a'),(NULL,'unknown')");
            expect(ErrorCode::constraint, [&] { c.execute("UPDATE composite SET b='b'"); });
            c.execute("CREATE TABLE tree(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES tree(id))");
            c.execute("INSERT INTO tree VALUES(1,1),(2,1)");
            expect(ErrorCode::constraint, [&] { c.execute("DELETE FROM tree WHERE id=1"); });
            c.execute("DELETE FROM tree"); // Self-references disappear in the same native mutation.
            c.execute("ALTER TABLE models RENAME TO descriptors");
            c.execute("ALTER TABLE descriptors RENAME COLUMN id TO key");
            c.execute("ALTER TABLE frames RENAME COLUMN model TO descriptor");
            c.execute("ALTER TABLE frames RENAME COLUMN seen TO sequence");
            expect(ErrorCode::constraint, [&] { c.execute("UPDATE frames SET sequence=-1"); });
            expect(ErrorCode::constraint, [&] { c.execute("DELETE FROM descriptors"); });
            expect(ErrorCode::unsupported,
                   [&] { c.execute("CREATE TABLE bad(a INTEGER CHECK(a>(SELECT 1)))"); });
            CHECK(!db.schema().contains("bad"));
            expect(ErrorCode::unsupported,
                   [&] { c.execute("CREATE TABLE bad(a INTEGER CHECK(a>?))", Row{std::int64_t{0}}); });
            c.execute("CREATE TABLE plain(id INTEGER)");
            c.execute("CREATE UNIQUE INDEX unique_parent ON plain(id)");
            c.execute("CREATE TABLE ref(id INTEGER REFERENCES plain(id))");
            expect(ErrorCode::schema, [&] { c.execute("DROP INDEX unique_parent"); });
            db.begin().integrity_check();
            db.save(temp.path / "snapshot");
            db.checkpoint();
            c.execute("INSERT INTO frames VALUES(3,1,5)");
            db.backup(temp.path / "backup");
        }
        for (const auto& file : {"db", "backup"}) {
            auto db = Database::open(temp.path / file, registry);
            sql::Connection c(db, registry);
            CHECK(c.execute("SELECT count(*) FROM frames").rows == std::vector<Row>{{std::int64_t{3}}});
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO frames VALUES(4,2,0)"); });
            expect(ErrorCode::constraint, [&] { c.execute("UPDATE frames SET sequence=-5"); });
            expect(ErrorCode::constraint, [&] { c.execute("DELETE FROM descriptors"); });
            db.begin().integrity_check();
        }
        auto snapshot = Database::load(temp.path / "snapshot", registry);
        sql::Connection c(snapshot, registry);
        expect(ErrorCode::constraint, [&] { c.execute("UPDATE frames SET sequence=-1"); });
        expect(ErrorCode::constraint, [&] { c.execute("DELETE FROM descriptors"); });
        snapshot.begin().integrity_check();
    });
}
