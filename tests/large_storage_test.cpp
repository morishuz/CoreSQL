#include "check.hpp"
using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        constexpr std::size_t rows = 72, width = 1024 * 1024;
        CHECK(Database::encoded_size_limit() >= 128 * 1024 * 1024);
        auto path = temp.path / "large";
        {
            auto db = Database::open(path);
            auto tx = db.begin();
            tx.create_table("data", {{"id", integer(), true}, {"payload", text()}});
            for (std::size_t i = 0; i < rows; ++i)
                tx.insert("data", {static_cast<std::int64_t>(i),
                                   std::string(width, static_cast<char>('a' + i % 26))});
            tx.commit();
            CHECK(std::filesystem::file_size(path) > 64 * 1024 * 1024);
            db.checkpoint();
            db.backup(temp.path / "backup");
            db.save(temp.path / "snapshot");
        }
        for (const auto& file : {path, temp.path / "backup"}) {
            auto db = Database::open(file);
            CHECK(db.stats().rows == rows);
            auto result = db.query(
                Query{"data", {}, Predicate{column("id"), Compare::equal, literal(std::int64_t{71})}});
            CHECK(std::get<std::string>(result.rows[0][1]) == std::string(width, 't'));
            db.begin().integrity_check();
        }
        auto db = Database::restore(temp.path / "snapshot", temp.path / "restored");
        CHECK(db.stats().rows == rows);
        db.begin().integrity_check();
    });
}
