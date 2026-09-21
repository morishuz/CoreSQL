#include "check.hpp"
#include "coresql/sql.hpp"
#include "../src/storage.hpp"
#include <cstring>
#include <sys/wait.h>
using namespace coresql;
namespace {
const char* stop_stage = nullptr;
void crash(const char* stage) {
    if (std::strcmp(stage, stop_stage) == 0)
        ::_exit(77);
}
} // namespace
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
        for (const char* stage : {"half_payload", "payload_synced", "half_footer", "footer", "synced"}) {
            const auto file = temp.path / stage;
            {
                auto db = Database::open(file, registry);
                sql::Connection c(db, registry);
                c.execute("CREATE TABLE parent(id INTEGER PRIMARY KEY)");
                c.execute("INSERT INTO parent VALUES(1)");
                db.checkpoint();
            }
            const auto child = ::fork();
            CHECK(child >= 0);
            if (!child) {
                try {
                    auto db = Database::open(file, registry);
                    sql::Connection c(db, registry);
                    c.execute("BEGIN");
                    c.execute(
                        "CREATE TABLE child(id INTEGER CHECK(id>0),parent INTEGER REFERENCES parent(id))");
                    c.execute("INSERT INTO child VALUES(1,1)");
                    stop_stage = stage;
                    detail::storage_hook(crash);
                    c.execute("COMMIT");
                    ::_exit(78);
                } catch (...) {
                    ::_exit(79);
                }
            }
            int status = 0;
            CHECK(::waitpid(child, &status, 0) == child);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 77);
            auto db = Database::open(file, registry);
            sql::Connection c(db, registry);
            const bool published = std::strcmp(stage, "footer") == 0 || std::strcmp(stage, "synced") == 0;
            CHECK(db.schema().contains("child") == published);
            if (!published) {
                c.execute("CREATE TABLE child(id INTEGER CHECK(id>0),parent INTEGER REFERENCES parent(id))");
                c.execute("INSERT INTO child VALUES(1,1)");
            }
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO child VALUES(-1,1)"); });
            expect(ErrorCode::constraint, [&] { c.execute("INSERT INTO child VALUES(2,9)"); });
            expect(ErrorCode::constraint, [&] { c.execute("DELETE FROM parent"); });
            db.begin().integrity_check();
        }
        // Even a checksum-valid export cannot import data violating its checks.
        detail::State invalid;
        auto table = std::make_shared<detail::Table>();
        table->columns = {{"n", integer()}};
        table->constraints.checks = {{"false", literal(std::int64_t{0})}};
        auto chunk = std::make_shared<detail::Chunk>();
        chunk->rows = {{std::int64_t{1}}};
        chunk->rowids = {1};
        detail::refresh(*chunk);
        table->chunks.emplace(0, chunk);
        table->next_chunk = 1;
        table->next_rowid = 2;
        detail::refresh(*table);
        invalid.tables.emplace("invalid", table);
        auto bytes = detail::encode(invalid);
        const auto file = temp.path / "invalid.snapshot";
        {
            std::ofstream output(file, std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            CHECK(bool(output));
        }
        expect(ErrorCode::constraint, [&] { Database::load(file, registry); });
    });
}
