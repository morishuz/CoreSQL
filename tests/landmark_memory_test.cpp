#include "check.hpp"
#include "../examples/landmark_memory/workload.hpp"
int main() {
    return tests([] {
        using namespace coresql;
        TempDirectory temp;
        auto registry = landmarks::registry();
        {
            auto db = Database::open(temp.path / "memory", registry);
            sql::Connection c(db, registry);
            c.execute("BEGIN");
            landmarks::create(c);
            landmarks::insert(c, 0, 1000);
            c.execute("COMMIT");
            for (std::int64_t id : {0, 1, 551, 998, 999})
                landmarks::check(
                    c.execute(landmarks::search_statement(), landmarks::search_parameters(id)).rows,
                    landmarks::oracle(1000, id));
            c.execute("BEGIN");
            c.execute("UPDATE landmarks SET seen=42 WHERE id=1");
            expect(ErrorCode::type,
                   [&] { c.execute("UPDATE landmarks SET descriptor='[1,2]' WHERE id>=0"); });
            landmarks::check(c.execute(landmarks::search_statement(), landmarks::search_parameters(1)).rows,
                             landmarks::oracle(1000, 1));
            c.execute("SAVEPOINT frame");
            c.execute("DELETE FROM landmarks WHERE id=551");
            c.execute("ROLLBACK TO frame");
            c.execute("RELEASE frame");
            c.execute("COMMIT");
            db.checkpoint();
            c.execute("UPDATE landmarks SET seen=43 WHERE id=1");
            db.backup(temp.path / "backup");
        }
        for (const auto& name : {"memory", "backup"}) {
            auto db = Database::open(temp.path / name, registry);
            sql::Connection c(db, registry);
            landmarks::check(c.execute("SELECT seen FROM landmarks WHERE id=1").rows, {{std::int64_t{43}}});
            landmarks::check(c.execute(landmarks::search_statement(), landmarks::search_parameters(551)).rows,
                             landmarks::oracle(1000, 551));
            db.begin().integrity_check();
        }
    });
}
