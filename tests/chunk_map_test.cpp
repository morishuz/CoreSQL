#include "check.hpp"
#include "../src/storage.hpp"
#include <map>
#include <random>

using namespace coresql;
using namespace coresql::detail;

static std::shared_ptr<Chunk> chunk(std::int64_t value) {
    auto result = std::make_shared<Chunk>();
    result->rows.push_back({value});
    result->rowids.push_back(value);
    refresh(*result);
    return result;
}

int main() {
    return tests([] {
        ChunkMap map;
        for (std::uint64_t id = 0; id < 257; ++id)
            map.emplace(id, chunk(static_cast<std::int64_t>(id)));
        auto snapshot = map;
        // Copies share metadata without incrementing every chunk's ownership.
        CHECK(map.find(0)->second.pin().use_count() == 2);
        map[64].writable()->rows[0][0] = std::int64_t{-1};
        CHECK(snapshot.find(64)->second.pin()->rows[0][0] == Value(std::int64_t{64}));
        CHECK(map.find(64)->second.pin()->rows[0][0] == Value(std::int64_t{-1}));
        CHECK(map.find(0)->second.pin().use_count() == 2);
        // Reserve may leave an empty block after a failed insertion. Iteration,
        // reverse iteration, and subsequent copies must still see only entries.
        map.reserve_insert(10000);
        CHECK(map.rbegin()->first == 256);
        CHECK(std::distance(map.begin(), map.end()) == 257);
        map = snapshot;
        std::map<std::uint64_t, std::int64_t> expected;
        for (std::uint64_t id = 0; id < 257; ++id)
            expected.emplace(id, static_cast<std::int64_t>(id));
        std::mt19937 generator(13);
        for (int step = 0; step < 2000; ++step) {
            const auto id = generator() % 1000;
            if (step % 3 == 0) {
                CHECK(map.erase(id) == expected.erase(id));
            } else if (step % 3 == 1) {
                map[id] = chunk(step);
                expected[id] = step;
            } else if (map.contains(id)) {
                map[id].reset();
                map.remove_empty();
                expected.erase(id);
            }
            CHECK(map.size() == expected.size());
            auto actual = map.begin();
            for (const auto& [key, value] : expected) {
                CHECK(actual != map.end() && actual->first == key);
                CHECK(actual->second.pin()->rows[0][0] == Value(value));
                ++actual;
            }
            CHECK(actual == map.end());
            if (!expected.empty())
                CHECK(map.rbegin()->first == expected.rbegin()->first);
        }
        CHECK(snapshot.size() == 257);
        for (const auto& [id, ref] : snapshot)
            CHECK(ref.pin()->rows[0][0] == Value(static_cast<std::int64_t>(id)));
        map[UINT64_MAX] = chunk(3);
        CHECK(map.rbegin()->first == UINT64_MAX);
        CHECK(map.erase(UINT64_MAX) == 1);
    });
}
