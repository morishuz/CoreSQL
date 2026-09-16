#include "check.hpp"
#include "../src/storage.hpp"

using namespace coresql;

int main() {
    return tests([] {
        // This translation unit and the separately compiled library must agree.
        const auto limit = Database::encoded_size_limit();
        CHECK(limit == detail::max_encoded_bytes);
        TempDirectory temp;
        const auto path = temp.path / "live";
        {
            auto db = Database::open(path);
            auto tx = db.begin();
            tx.create_table("data", {{"payload", text()}});
            tx.insert("data", {std::string("retained")});
            tx.commit();
            // Small-limit builds exercise the boundary without allocating GiB in
            // the ordinary suite. Run with an 8 MiB configuration to check rejection.
            if (limit <= 16 * 1024 * 1024) {
                auto oversized = db.begin();
                oversized.insert("data", {std::string(limit, 'x')});
                expect(ErrorCode::format, [&] { oversized.commit(); });
                oversized.rollback();
                CHECK(db.stats().rows == 1);
                // Rejection must not poison the owner or publish partial data.
                auto next = db.begin();
                next.insert("data", {std::string("after rejection")});
                next.commit();
            }
            db.checkpoint();
            db.backup(temp.path / "backup");
        }
        for (const auto& file : {path, temp.path / "backup"}) {
            auto db = Database::open(file);
            const auto rows = db.query(Query{"data"}).rows;
            CHECK(rows.size() == (limit <= 16 * 1024 * 1024 ? 2 : 1));
            CHECK(std::get<std::string>(rows[0][0]) == "retained");
            if (rows.size() == 2)
                CHECK(std::get<std::string>(rows[1][0]) == "after rejection");
            db.begin().integrity_check();
        }
    });
}
