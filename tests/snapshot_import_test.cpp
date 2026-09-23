#include "check.hpp"
#include "../src/storage.hpp"
using namespace coresql;
namespace {
detail::State state(std::vector<std::int64_t> ids, std::int64_t next) {
    detail::State result;
    auto table = std::make_shared<detail::Table>();
    table->columns = {{"key", integer(), true}, {"body", text()}};
    table->index_definitions = {{"by_body", {"body"}}};
    table->next_rowid = next;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i % 128 == 0)
            table->chunks.emplace(table->next_chunk++, std::make_shared<detail::Chunk>());
        auto& chunk = *table->chunks[table->chunks.rbegin()->first].writable();
        chunk.rowids.push_back(ids[i]);
        chunk.rows.push_back({static_cast<std::int64_t>(i + 1), std::string(i % 2 ? 3 : 700, 'x')});
    }
    for (auto& [id, chunk] : table->chunks) {
        (void)id;
        detail::refresh(*chunk.pin());
    }
    detail::refresh(*table);
    result.tables["t"] = std::move(table);
    return result;
}
void save(const detail::State& state, const std::filesystem::path& path) {
    auto data = detail::encode(state);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    CHECK(bool(file));
}
} // namespace
int main() {
    return tests([] {
        TempDirectory temp;
        auto check = [&](std::vector<std::int64_t> ids, std::int64_t next) {
            auto original = state(ids, next);
            auto path = temp.path / "snapshot";
            save(original, path);
            auto db = Database::load(path);
            auto tx = db.begin();
            tx.integrity_check();
            auto rows = tx.query(Query{"t", {row_id(), column("key"), column("body")}}).rows;
            CHECK(rows.size() == ids.size());
            for (std::size_t i = 0; i < ids.size(); ++i) {
                CHECK(rows[i][0] == Value(ids[i]));
                CHECK(rows[i][1] == Value(static_cast<std::int64_t>(i + 1)));
                CHECK(rows[i][2] == Value(std::string(i % 2 ? 3 : 700, 'x')));
            }
            if (ids.size() > 1) {
                Query lookup{
                    "t", {row_id()}, Predicate{column("key"), Compare::equal, literal(std::int64_t{2})}};
                CHECK(tx.query(lookup).rows[0][0] == Value(ids[1]));
            }
            Row added{static_cast<std::int64_t>(ids.size() + 1), std::string("new")};
            if (next == INT64_MAX)
                expect(ErrorCode::state, [&] { tx.insert("t", added); });
            else {
                tx.insert("t", added);
                CHECK(tx.query(Query{"t", {row_id()}}).rows.back()[0] == Value(next));
            }
            tx.integrity_check();
        };
        check({9, 2, 7}, 50);
        std::vector<std::int64_t> many;
        for (std::int64_t i = 300; i > 0; --i)
            many.push_back(i * 2);
        check(many, 900); // Import uses different physical chunk boundaries.
        check({INT64_MAX - 1, 1}, INT64_MAX);
        check({}, INT64_MAX);
        for (auto ids : {std::vector<std::int64_t>{1, 1}, {0}, {9}, {INT64_MAX}}) {
            auto path = temp.path / "invalid";
            save(state(ids, 9), path);
            expect(ErrorCode::format, [&] { Database::load(path); });
        }
    });
}
