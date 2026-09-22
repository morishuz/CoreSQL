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
std::int64_t id(const sql::Result& result) {
    CHECK(result.rows.size() == 1);
    return std::get<std::int64_t>(result.rows[0][0]);
}
int main() {
    return tests([] {
        TempDirectory temp;
        auto path = temp.path / "ids";
        {
            auto db = Database::open(path, registry());
            sql::Connection c(db, registry());
            c.execute("CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT UNIQUE)");
            auto first = c.execute("INSERT INTO notes(body) VALUES ('one') RETURNING id AS new_id, body");
            CHECK(id(first) == 1 && first.changes == 1 &&
                  first.columns == std::vector<std::string>({"new_id", "body"}));
            CHECK(id(c.execute("INSERT INTO notes VALUES(NULL, 'two') RETURNING notes.id")) == 2);
            auto many = c.execute("INSERT INTO notes(body) VALUES ('three'), ('four') RETURNING *");
            CHECK(many.changes == 2 && many.rows.size() == 2 && many.rows[1][0] == Value(std::int64_t{4}));
            c.execute("INSERT INTO notes VALUES (100, 'explicit')");
            c.execute("BEGIN");
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('rolled') RETURNING id")) == 101);
            c.execute("ROLLBACK");
            c.execute("SAVEPOINT outer");
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('rewound') RETURNING id")) == 101);
            c.execute("ROLLBACK TO outer");
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('kept') RETURNING id")) == 101);
            c.execute("RELEASE outer");
            c.execute("BEGIN");
            expect(ErrorCode::constraint,
                   [&] { c.execute("INSERT INTO notes(body) VALUES('partial'), ('one') RETURNING id"); });
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('after_failure') RETURNING id")) == 102);
            c.execute("COMMIT");
            c.execute("DELETE FROM notes WHERE id=102");
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('reused') RETURNING id")) == 102);
            auto before = db.stats().rows;
            expect(ErrorCode::schema,
                   [&] { c.execute("INSERT INTO notes(body) VALUES('bad') RETURNING missing"); });
            expect(ErrorCode::unsupported,
                   [&] { c.execute("INSERT INTO notes(body) VALUES('bad') RETURNING id+1"); });
            CHECK(db.stats().rows == before);
            c.execute("CREATE TABLE source (text TEXT)");
            c.execute("INSERT INTO source VALUES('from_select_a'), ('from_select_b')");
            auto copied = c.execute("INSERT INTO notes(body) SELECT text FROM source RETURNING id");
            CHECK(copied.rows.size() == 2 && copied.rows[0][0] == Value(std::int64_t{103}));
            auto empty = c.execute("INSERT INTO notes(body) SELECT text FROM source WHERE 0 RETURNING id");
            CHECK(empty.rows.empty() && empty.columns == std::vector<std::string>{"id"});
            expect(ErrorCode::schema, [&] {
                c.execute("INSERT INTO notes(body) SELECT text FROM source WHERE 0 RETURNING missing");
            });
            c.execute("CREATE TABLE defaults(id BIGINT PRIMARY KEY, body TEXT DEFAULT 'default')");
            CHECK(id(c.execute("INSERT INTO defaults DEFAULT VALUES RETURNING id")) == 1);
            const sql::Statement prepared(
                "INSERT INTO defaults(body) VALUES(?) RETURNING id, body AS content");
            CHECK(id(c.execute(prepared, Row{std::string("parameter")})) == 2);
            c.execute("CREATE TABLE strings(id TEXT PRIMARY KEY)");
            expect(ErrorCode::constraint,
                   [&] { c.execute("INSERT INTO strings DEFAULT VALUES RETURNING id"); });
            c.execute("CREATE TABLE extremes(id INT PRIMARY KEY)");
            c.execute("INSERT INTO extremes VALUES(-10), (0)");
            CHECK(id(c.execute("INSERT INTO extremes DEFAULT VALUES RETURNING id")) == 1);
            c.execute("INSERT INTO extremes VALUES(9223372036854775807)");
            expect(ErrorCode::constraint,
                   [&] { c.execute("INSERT INTO extremes DEFAULT VALUES RETURNING id"); });
            c.execute("CREATE TABLE mixed(id INTEGER PRIMARY KEY)");
            auto mix = c.execute("INSERT INTO mixed VALUES(NULL), (10), (NULL) RETURNING id");
            CHECK(mix.rows[0][0] == Value(std::int64_t{1}) && mix.rows[2][0] == Value(std::int64_t{11}));
            c.execute("UPDATE mixed SET id=20 WHERE id=11");
            CHECK(id(c.execute("INSERT INTO mixed DEFAULT VALUES RETURNING id")) == 21);
            db.checkpoint();
            db.backup(temp.path / "backup");
            db.save(temp.path / "snapshot");
            c.execute("PRAGMA integrity_check");
        }
        for (const auto& file : {path, temp.path / "backup"}) {
            auto db = Database::open(file, registry());
            sql::Connection c(db, registry());
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('reopened') RETURNING id")) == 105);
            // A competing transaction cannot publish the same generated ID twice.
            sql::Connection other(db, registry());
            c.execute("BEGIN");
            other.execute("BEGIN");
            CHECK(id(c.execute("INSERT INTO defaults(body) VALUES('a') RETURNING id")) == 3);
            CHECK(id(other.execute("INSERT INTO defaults(body) VALUES('b') RETURNING id")) == 3);
            c.execute("COMMIT");
            expect(ErrorCode::conflict, [&] { other.execute("COMMIT"); });
            other.execute("ROLLBACK");
            CHECK(id(other.execute("INSERT INTO defaults(body) VALUES('b') RETURNING id")) == 4);
        }
        {
            auto restored = Database::restore(temp.path / "snapshot", temp.path / "restored", registry());
            sql::Connection c(restored, registry());
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('restored') RETURNING id")) == 105);
        }
        for (const auto* stage : {"payload_synced", "synced"}) {
            auto file = temp.path / stage;
            std::filesystem::copy_file(path, file);
            auto pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                auto db = Database::open(file, registry());
                sql::Connection c(db, registry());
                static const char* stop;
                stop = stage;
                detail::storage_hook([](const char* at) {
                    if (std::strcmp(at, stop) == 0)
                        _exit(77);
                });
                c.execute("INSERT INTO notes(body) VALUES('crash') RETURNING id");
                _exit(78);
            }
            int status;
            CHECK(waitpid(pid, &status, 0) == pid);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
            auto db = Database::open(file, registry());
            sql::Connection c(db, registry());
            CHECK(id(c.execute("INSERT INTO notes(body) VALUES('after_crash') RETURNING id")) ==
                  (std::strcmp(stage, "synced") == 0 ? 107 : 106));
            c.execute("PRAGMA integrity_check");
        }
        {
            auto db = Database::open(temp.path / "remembered", registry());
            sql::Connection c(db, registry());
            c.execute("CREATE TABLE keys(id INTEGER PRIMARY KEY, other INTEGER UNIQUE)");
            c.execute("INSERT INTO keys VALUES(1,1),(5,5)");
            detail::visited_chunks() = 0;
            CHECK(id(c.execute("INSERT INTO keys(other) VALUES(2) RETURNING id")) == 6);
            CHECK(detail::visited_chunks() == 0);
            c.execute("DELETE FROM keys WHERE id=1");
            detail::visited_chunks() = 0;
            CHECK(id(c.execute("INSERT INTO keys(other) VALUES(3) RETURNING id")) == 7);
            CHECK(detail::visited_chunks() == 0);
            c.execute("DELETE FROM keys WHERE id=7");
            detail::visited_chunks() = 0;
            CHECK(id(c.execute("INSERT INTO keys(other) VALUES(4) RETURNING id")) == 7);
            CHECK(detail::visited_chunks() > 0);
            c.execute("UPDATE keys SET id=4 WHERE id=7");
            detail::visited_chunks() = 0;
            CHECK(id(c.execute("INSERT INTO keys(other) VALUES(8) RETURNING id")) == 7);
            CHECK(detail::visited_chunks() > 0);
            c.execute("UPDATE keys SET id=2 WHERE id=5");
            CHECK(id(c.execute("INSERT INTO keys(other) VALUES(6) RETURNING id")) == 8);
            c.execute("UPDATE keys SET id=9 WHERE id=6");
            CHECK(id(c.execute("INSERT INTO keys(other) VALUES(9) RETURNING id")) == 10);
            c.execute("DELETE FROM keys");
            CHECK(id(c.execute("INSERT INTO keys(other) VALUES(1) RETURNING id")) == 1);
            db.checkpoint();
        }
        {
            auto reopened = Database::open(temp.path / "remembered", registry());
            sql::Connection again(reopened, registry());
            detail::visited_chunks() = 0;
            CHECK(id(again.execute("INSERT INTO keys(other) VALUES(2) RETURNING id")) == 2);
            CHECK(detail::visited_chunks() > 0);
            detail::visited_chunks() = 0;
            CHECK(id(again.execute("INSERT INTO keys(other) VALUES(3) RETURNING id")) == 3);
            CHECK(detail::visited_chunks() == 0);
        }
    });
}
