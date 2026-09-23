#include "check.hpp"
#include "../src/storage.hpp"
#include "../src/pager.hpp"

using namespace coresql;
using namespace coresql::detail;

int main() {
    return tests([] {
        Registry registry;
        State base;
        auto table = std::make_shared<Table>();
        table->columns = {{"value", text()}};
        for (std::uint64_t id = 0; id < 256; ++id) {
            auto chunk = std::make_shared<Chunk>();
            chunk->rows = {{std::to_string(id)}};
            chunk->rowids = {static_cast<std::int64_t>(id + 1)};
            refresh(*chunk);
            table->chunks.emplace(id, chunk);
        }
        table->next_chunk = 256;
        table->next_rowid = 257;
        refresh(*table);
        base.tables["t"] = table;
        const auto base_size = checkpoint_size(base);
        auto pager = std::make_shared<Pager>(32768, registry);
        page_state(base, {}, pager);
        CHECK(pager->stats().page_writes == 256);
        CHECK(checkpoint_size(base) == base_size);
        State next = base;
        next.tables["t"] = std::make_shared<Table>(*base.tables.at("t"));
        auto& edited = *next.tables.at("t");
        for (std::uint64_t id = 64; id < 128; ++id)
            CHECK(edited.chunks.erase(id) == 1);
        auto& changed = edited.chunks[128].writable();
        changed->rows[0][0] = std::string(40000, 'x');
        refresh(*changed);
        auto added = std::make_shared<Chunk>();
        added->rows = {{std::string("added")}};
        added->rowids = {257};
        refresh(*added);
        edited.chunks.emplace(1024, added);
        edited.next_chunk = 1025;
        edited.next_rowid = 258;
        refresh(edited);
        CHECK(edited.chunks.changes_from(base.tables.at("t")->chunks).size() == 66);
        const auto bytes = checkpoint_size(next);
        CHECK(bytes > base_size);
        page_state(next, base, pager);
        CHECK(pager->stats().page_writes == 258);
        CHECK(checkpoint_size(next) == bytes);
        const auto record = encode_changes(base, next);
        auto recovered = apply_changes(base, record, registry);
        const auto& actual = *recovered.tables.at("t");
        CHECK(actual.chunks.size() == next.tables.at("t")->chunks.size());
        for (const auto& [id, ref] : next.tables.at("t")->chunks) {
            const auto found = actual.chunks.find(id);
            CHECK(found != actual.chunks.end());
            CHECK(found->second.pin()->rows == ref.pin()->rows);
            CHECK(found->second.pin()->rowids == ref.pin()->rowids);
        }
        CHECK(checkpoint_size(recovered) == bytes);
        CHECK(checkpoint_size(base) == base_size);
        // A rollback is simply the earlier snapshot, including cached sizes and
        // shared-block identity. It cannot leak edits into a later change record.
        next = base;
        CHECK(next.tables.at("t")->chunks.changes_from(base.tables.at("t")->chunks).empty());
        page_state(next, base, pager);
        CHECK(pager->stats().page_writes == 258);
    });
}
