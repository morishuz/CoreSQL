#include "check.hpp"
#include "../src/storage.hpp"
#include "coresql/encoding.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
namespace {
void field(Bytes& out, std::string_view s) {
    encoding::u64(out, s.size());
    auto bytes = std::as_bytes(std::span(s.data(), s.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}
// Independent legacy/current wire fixture: one empty INTEGER table with one index.
Bytes fixture(bool log, bool legacy, std::string_view name, std::uint64_t flags) {
    Bytes out;
    field(out, log ? (legacy ? "CORECHG7" : "CORECHG8") : (legacy ? "CORESQL6" : "CORESQL7"));
    encoding::u64(out, 1); // tables
    field(out, "t");
    encoding::u64(out, 1); // columns
    field(out, "k");
    field(out, integer().id);
    encoding::u64(out, integer().version);
    field(out, ""); // type parameters
    for (int i = 0; i < 2; ++i)
        encoding::u64(out, 0); // primary, nullable
    field(out, "");            // index implementation
    encoding::u64(out, 0);     // default
    encoding::u64(out, 1);     // named indexes
    field(out, name);
    encoding::u64(out, flags);
    encoding::u64(out, 1); // index width
    field(out, "k");
    encoding::u64(out, 0); // ascending
    encoding::u64(out, 0); // checks
    encoding::u64(out, 0); // foreign keys
    if (log)
        encoding::u64(out, 0); // next chunk
    encoding::u64(out, 1);     // next rowid
    encoding::u64(out, 0);     // rows/chunks
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto byte : out) {
        hash ^= std::to_integer<unsigned>(byte);
        hash *= 1099511628211ULL;
    }
    encoding::u64(out, hash);
    return out;
}
void save(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(bool(out));
}
void check(Database& db) {
    auto tx = db.begin();
    CHECK(tx.indexes("t").size() == 2);
    for (const auto& index : tx.indexes("t")) {
        CHECK(index.constraint_owned == (index.name == "owned"));
        if (index.constraint_owned)
            expect(ErrorCode::constraint, [&] { tx.drop_index("t", index.name); });
        else
            tx.drop_index("t", index.name);
    }
    expect(ErrorCode::constraint, [&] { tx.insert("t", {std::int64_t{1}}); });
    tx.rollback();
}
} // namespace
int main() {
    return tests([] {
        TempDirectory temp;
        Registry registry;
        sql::install(registry);
        OpenOptions options;
        options.concurrent_reads = true;
        const auto path = temp.path / "owned.db";
        {
            auto db = Database::open(path, registry, options);
            auto tx = db.begin();
            tx.create_table("t", {{"k", integer()}});
            tx.insert("t", {std::int64_t{1}});
            tx.create_index("t", {"sql.unique.core", {"k"}, true});
            tx.create_index("t", {"owned", {"k"}, true, {}, true});
            expect(ErrorCode::schema, [&] { tx.create_index("t", {"invalid", {"k"}, false, {}, true}); });
            tx.commit();
            check(db);
            sql::Connection c(db, registry);
            c.execute("CREATE TABLE constraints(a INTEGER UNIQUE, b INTEGER, c INTEGER, UNIQUE(a,b), PRIMARY "
                      "KEY(b,c))");
            CHECK(db.begin().indexes("constraints").size() == 3);
            for (const auto& d : db.begin().indexes("constraints")) {
                CHECK(d.constraint_owned);
                expect(ErrorCode::constraint, [&] { db.begin().drop_index("constraints", d.name); });
            }
        }
        for (int checkpoint = 0; checkpoint < 3; ++checkpoint) {
            auto db = Database::open(path, registry, options);
            check(db);
            for (const auto& d : db.begin().indexes("constraints"))
                CHECK(d.constraint_owned);
            if (checkpoint == 0)
                db.checkpoint();
            if (checkpoint == 1)
                db.checkpoint_async().get();
            if (checkpoint == 2) {
                db.save(temp.path / "export");
                auto exported = Database::load(temp.path / "export", registry);
                check(exported);
            }
        }
        for (bool legacy : {false, true})
            for (const auto& name : {std::string("sql.unique.k"), std::string("ordinary")}) {
                const bool owned = legacy && name.starts_with("sql.unique.");
                save(temp.path / "fixture", fixture(false, legacy, name, 1));
                auto db = Database::load(temp.path / "fixture", registry);
                CHECK(db.begin().indexes("t")[0].constraint_owned == owned);
                if (owned)
                    expect(ErrorCode::constraint, [&] { db.begin().drop_index("t", name); });
                else
                    db.begin().drop_index("t", name);
                auto state = detail::apply_changes({}, fixture(true, legacy, name, 1), registry);
                CHECK(state.tables.at("t")->index_definitions[0].constraint_owned == owned);
                // A current record appended to legacy state retains the migrated bit.
                auto roundtrip = detail::apply_changes(state, detail::encode_changes({}, state), registry);
                CHECK(roundtrip.tables.at("t")->index_definitions[0].constraint_owned == owned);
            }
        for (bool legacy : {false, true})
            for (auto flags : {2u, 4u}) {
                save(temp.path / "bad", fixture(false, legacy, "bad", flags));
                expect(ErrorCode::format, [&] { Database::load(temp.path / "bad", registry); });
                expect(ErrorCode::format,
                       [&] { detail::apply_changes({}, fixture(true, legacy, "bad", flags), registry); });
            }
    });
}
