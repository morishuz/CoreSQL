#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/storage.hpp"
#include <cstring>
#include <sys/wait.h>
using namespace coresql;
Registry registry() {
    Registry r;
    sql::install(r);
    return r;
}
int main() {
    return tests([] {
        TempDirectory temp;
        auto path = temp.path / "live";
        {
            auto db = Database::open(path, registry());
            sql::Connection c(db, registry());
            c.execute("CREATE TABLE IF NOT EXISTS t (id INTEGER PRIMARY KEY, value TEXT DEFAULT 'x')");
            c.execute("CREATE TABLE IF NOT EXISTS t (ignored INTEGER)");
            c.execute("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')");
            c.execute("CREATE INDEX IF NOT EXISTS ix ON t(value)");
            c.execute("CREATE INDEX IF NOT EXISTS ix ON t(value)");
            auto old = db.begin();
            c.execute("BEGIN");
            c.execute("ALTER TABLE t RENAME COLUMN value TO label");
            c.execute("ALTER TABLE t RENAME TO renamed");
            c.execute("ROLLBACK");
            CHECK(db.schema().contains("t"));
            c.execute("ALTER TABLE t RENAME COLUMN value TO label");
            c.execute("ALTER TABLE t RENAME TO renamed");
            db.backup(temp.path / "renamed-index");
            {
                auto check = Database::open(temp.path / "renamed-index", registry());
                auto tx = check.begin();
                CHECK(tx.indexes("renamed")[0].columns == std::vector<std::string>{"label"});
                tx.integrity_check();
            }
            CHECK(old.query(Query{"t"}).rows.size() == 3);
            old.rollback();
            auto result = c.execute("SELECT id, label AS name FROM renamed ORDER BY id LIMIT 1 OFFSET 1");
            CHECK(result.rows.size() == 1 && result.rows[0][0] == Value(std::int64_t{2}));
            CHECK(result.columns == std::vector<std::string>({"id", "name"}));
            CHECK(c.execute("SELECT * FROM renamed LIMIT 0 OFFSET 1").rows.empty());
            CHECK(c.execute("SELECT * FROM renamed LIMIT 2 OFFSET 99").rows.empty());
            CHECK(c.execute("SELECT id FROM renamed WHERE id IN (SELECT id FROM renamed ORDER BY id LIMIT 1 "
                            "OFFSET 1)")
                      .rows.size() == 1);
            c.execute("SAVEPOINT outer");
            c.execute("DELETE FROM renamed WHERE id=1");
            c.execute("SAVEPOINT inner");
            c.execute("DROP TABLE renamed");
            c.execute("ROLLBACK TO outer");
            CHECK(c.execute("SELECT * FROM renamed").rows.size() == 3);
            c.execute("ROLLBACK TO outer");
            c.execute("RELEASE outer");
            CHECK(!c.in_transaction());
            c.execute("BEGIN");
            c.execute("SAVEPOINT repeated");
            c.execute("DELETE FROM renamed WHERE id=1");
            c.execute("SAVEPOINT repeated");
            c.execute("DELETE FROM renamed WHERE id=2");
            c.execute("ROLLBACK TO repeated");
            CHECK(c.execute("SELECT * FROM renamed").rows.size() == 2);
            c.execute("COMMIT");
            c.execute("DROP INDEX ix");
            c.execute("DROP INDEX IF EXISTS ix");
            expect(ErrorCode::schema, [&] { c.execute("DROP INDEX ix"); });
            {
                sql::Connection abandoned(db, registry());
                abandoned.execute("SAVEPOINT abandoned");
                abandoned.execute("DROP TABLE renamed");
            }
            CHECK(db.schema().contains("renamed"));
            auto empty = c.execute("SELECT id AS identity FROM renamed WHERE id=999");
            CHECK(empty.rows.empty() && empty.columns == std::vector<std::string>{"identity"});
            CHECK(c.execute("SELECT id FROM renamed UNION SELECT 4 ORDER BY 1 LIMIT 1 OFFSET 1").rows[0][0] ==
                  Value(std::int64_t{3}));
            CHECK(c.execute("SELECT id, count(*) FROM renamed GROUP BY id ORDER BY id LIMIT 1 OFFSET 1")
                      .rows[0][0] == Value(std::int64_t{3}));
            expect(ErrorCode::unsupported, [&] { c.execute("SELECT * FROM renamed LIMIT 1 OFFSET -1"); });
            expect(ErrorCode::schema, [&] { c.execute("ALTER TABLE renamed RENAME COLUMN label TO id"); });
            c.execute("CREATE TABLE defaults (n INTEGER DEFAULT 7)");
            c.execute("INSERT INTO defaults DEFAULT VALUES");
            CHECK(c.execute("SELECT n FROM defaults").rows[0][0] == Value(std::int64_t{7}));
            c.execute("CREATE TABLE unique_t (n INTEGER UNIQUE)");
            expect(ErrorCode::constraint, [&] { c.execute("DROP INDEX \"sql.unique.n\""); });
            c.execute("DROP TABLE unique_t");
            c.execute("PRAGMA integrity_check");
            db.backup(temp.path / "backup");
            expect(ErrorCode::io, [&] { db.backup(temp.path / "backup"); });
            db.save(temp.path / "export");
            auto restored = Database::restore(temp.path / "export", temp.path / "restored", registry());
            CHECK(restored.query(Query{"renamed"}).rows.size() == 2);
            restored.begin().integrity_check();
            c.execute("BEGIN");
            c.execute("DROP TABLE renamed");
            c.execute("CREATE TABLE renamed (fresh TEXT)");
            c.execute("INSERT INTO renamed VALUES ('new')");
            c.execute("COMMIT");
        }
        {
            auto db = Database::open(path, registry());
            CHECK(db.schema().at("renamed")[0].name == "fresh");
            CHECK(db.query(Query{"renamed"}).rows[0][0] == Value(std::string("new")));
            auto backup = Database::open(temp.path / "backup", registry());
            CHECK(backup.query(Query{"renamed"}).rows.size() == 2);
            CHECK(backup.begin().indexes("renamed").empty());
        }
        // Destructive schema publication is atomic at the existing checkpoint boundary.
        for (const auto* stage : {"checkpoint_created", "checkpoint_synced", "checkpoint_renamed",
                                  "checkpoint_directory_synced"}) {
            auto childpath = temp.path / stage;
            std::filesystem::copy_file(path, childpath);
            auto pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                auto db = Database::open(childpath, registry());
                static const char* stop;
                stop = stage;
                detail::storage_hook([](const char* at) {
                    if (std::strcmp(at, stop) == 0)
                        _exit(77);
                });
                auto tx = db.begin();
                tx.drop_table("renamed");
                tx.commit();
                _exit(78);
            }
            int status;
            CHECK(waitpid(pid, &status, 0) == pid);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
            auto db = Database::open(childpath, registry());
            bool published = std::strcmp(stage, "checkpoint_renamed") == 0 ||
                             std::strcmp(stage, "checkpoint_directory_synced") == 0;
            CHECK(db.schema().contains("renamed") != published);
            db.begin().integrity_check();
        }
    });
}
