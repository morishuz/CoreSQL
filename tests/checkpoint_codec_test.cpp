#include "check.hpp"
#include "../src/storage.hpp"
#include "../src/pager.hpp"
#include "coresql/blob.hpp"
#include "coresql/date.hpp"
#include "coresql/decimal.hpp"
#include "coresql/sql.hpp"
#include "coresql/timestamp.hpp"
#include <bit>
using namespace coresql;
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
        OpenOptions options;
        options.concurrent_reads = true;
        timestamps::install(registry);
        const auto decimal = decimals::type(30, 4);
        const std::vector<Type> types{integer(),          real(),  text(),       dates::type(),
                                      timestamps::type(), decimal, blobs::type()};
        std::vector<Column> columns;
        for (std::size_t i = 0; i < types.size(); ++i)
            columns.push_back({"c" + std::to_string(i), types[i], false, {}, {}, true});
        std::vector<Row> rows{
            {INT64_MIN, -0.0, std::string("a\0b", 3), dates::value(-1), timestamps::value(INT64_MIN),
             decimals::parse("-12345678901234567890123456.7890", decimal),
             blobs::value(Bytes(200000, std::byte{0xab}))},
            {INT64_MAX, 1.25, std::string(200000, 'z'), dates::value(0), timestamps::value(INT64_MAX),
             decimals::parse("0.0000", decimal), blobs::value({})}};
        Row nulls;
        for (const auto& type : types)
            nulls.push_back(Null(type));
        rows.push_back(nulls);
        const auto path = temp.path / "mixed.db";
        {
            auto db = Database::open(path, registry, options);
            auto tx = db.begin();
            tx.create_table("mixed", columns);
            for (const auto& row : rows)
                tx.insert("mixed", row);
            tx.commit();
        }
        {
            detail::DurableStore store(path);
            auto state = store.recover(registry);
            Bytes streamed;
            std::size_t calls = 0;
            detail::encode_checkpoint(state, [&](ByteView part) {
                ++calls;
                streamed.insert(streamed.end(), part.begin(), part.end());
            });
            CHECK(calls > 3); // Large payloads pass through the streaming sink.
            CHECK(streamed == detail::encode_changes({}, state));
            CHECK(streamed.size() <= detail::checkpoint_size(state));
            for (const auto& [id, reference] : state.tables.at("mixed")->chunks) {
                (void)id;
                const auto chunk = reference.pin();
                const auto page = detail::encode_page(*chunk);
                CHECK(detail::decode_page(page, columns, registry)->rows == chunk->rows);
            }
        }
        auto verify = [&](Database& db) {
            auto result = db.query(Query{"mixed"});
            CHECK(result.rows == rows);
            CHECK(std::bit_cast<std::uint64_t>(std::get<double>(result.rows[0][1])) ==
                  std::bit_cast<std::uint64_t>(-0.0));
            db.begin().integrity_check();
        };
        {
            auto db = Database::open(path, registry, options);
            verify(db);
            db.checkpoint();
        }
        {
            auto db = Database::open(path, registry, options);
            verify(db);
            db.checkpoint_async().get();
            db.save(temp.path / "mixed.snapshot");
        }
        {
            auto db = Database::open(path, registry, options);
            verify(db);
            auto exported = Database::load(temp.path / "mixed.snapshot", registry);
            verify(exported);
        }
    });
}
