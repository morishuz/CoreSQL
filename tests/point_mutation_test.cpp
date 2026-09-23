#include "check.hpp"

using namespace coresql;

int main() {
    return tests([] {
        for (std::size_t cache : {std::size_t{0}, std::size_t{32768}}) {
            TempDirectory temp;
            Registry registry;
            Transaction* writing = nullptr;
            std::optional<QueryCursor> retained;
            int calls = 0;
            registry.add(Function{"fail", [](std::span<const Type>) { return integer(); },
                                  [&](std::span<const Value>) -> Value {
                                      ++calls;
                                      throw Error(ErrorCode::constraint, "assignment failed");
                                  }});
            registry.add(Function{"retain", [](std::span<const Type>) { return integer(); },
                                  [&](std::span<const Value> args) -> Value {
                                      retained.emplace(writing->cursor(Query{"t"}));
                                      return args[0];
                                  }});
            const auto path = temp.path / "points";
            const OpenOptions options{false, cache};
            std::optional<Database> db(Database::open(path, registry, options));
            {
                auto tx = db->begin();
                tx.create_table(
                    "t", {{"id", integer(), true}, {"a", integer()}, {"b", integer()}, {"note", text()}});
                for (std::int64_t i = 0; i < 300; ++i)
                    tx.insert("t", {i, i, i + 1, std::string("initial")});
                tx.commit();
            }
            db->checkpoint();
            db.reset();
            db.emplace(Database::open(path, registry, options));
            const Predicate key{column("id"), Compare::equal, literal(std::int64_t{150})};
            const Query point{"t", {}, key};
            {
                auto tx = db->begin();
                auto outcome =
                    tx.update_if("t", {{"a", call("fail", {})}}, key,
                                 Predicate{column("a"), Compare::less, literal(std::int64_t{0})}, true);
                CHECK(outcome.matched == 1 && outcome.updated == 0 && outcome.returning.rows.empty());
                CHECK(outcome.returning.types.size() == 4 && calls == 0);
                outcome = tx.update_if("t", {{"a", call("fail", {})}},
                                       Predicate{column("id"), Compare::equal, literal(std::int64_t{-1})},
                                       Predicate{call("fail", {}), Compare::equal, literal(std::int64_t{0})});
                CHECK(outcome.matched == 0 && outcome.updated == 0 && calls == 0);
                outcome = tx.update_if("t", {{"a", column("a")}}, key);
                CHECK(outcome.matched == 1 && outcome.updated == 1 && outcome.returning.rows.empty());
                outcome = tx.update_if("t", {{"a", column("b")}, {"b", column("a")}}, key, {}, true);
                CHECK(outcome.matched == 1 && outcome.updated == 1);
                CHECK(outcome.returning.rows == tx.query(point).rows);
                CHECK(outcome.returning.rows[0][1] == Value(std::int64_t{151}));
                CHECK(outcome.returning.rows[0][2] == Value(std::int64_t{150}));
                const auto before = tx.query(Query{"t"}).rows;
                expect(ErrorCode::constraint, [&] {
                    tx.update_if("t", {{"a", call("fail", {})}},
                                 Predicate{column("id"), Compare::greater, literal(std::int64_t{0})});
                });
                CHECK(tx.query(Query{"t"}).rows == before);
                calls = 0;
            }
            // A matching no-op still evaluates assignments and returns its row,
            // but must not publish a new snapshot or write pages/log records.
            {
                const auto bytes = db->storage_stats().bytes_written;
                const auto pages = db->cache_stats().page_writes;
                auto concurrent = db->begin();
                auto tx = db->begin();
                expect(ErrorCode::constraint,
                       [&] { tx.update("t", {{"a", column("a")}, {"b", call("fail", {})}}, key); });
                CHECK(calls == 1);
                calls = 0;
                auto result = tx.update_returning("t", {{"a", column("a")}}, key);
                CHECK(result.rows == tx.query(point).rows);
                tx.commit();
                CHECK(db->storage_stats().bytes_written == bytes);
                CHECK(db->cache_stats().page_writes == pages);
                concurrent.update("t", {{"note", literal(std::string("initial"))}}, key);
                concurrent.commit();
            }
            std::vector<Row> committed;
            {
                auto old = db->begin();
                auto tx = db->begin();
                writing = &tx;
                CHECK(tx.update("t", {{"a", column("b")}, {"b", column("a")}}, key) == 1);
                CHECK(tx.query(point).rows[0][1] == Value(std::int64_t{151}));
                CHECK(tx.query(point).rows[0][2] == Value(std::int64_t{150}));
                CHECK(old.query(point).rows[0][1] == Value(std::int64_t{150}));
                {
                    auto save = tx.savepoint();
                    CHECK(tx.update("t", {{"note", literal(std::string(20000, 'x'))}}, key) == 1);
                    save.rollback();
                }
                CHECK(tx.query(point).rows[0][3] == Value(std::string("initial")));
                const auto before = tx.query(point).rows;
                expect(ErrorCode::constraint, [&] {
                    tx.update("t", {{"a", literal(std::int64_t{999})}, {"b", call("fail", {})}}, key);
                });
                CHECK(calls == 1 && tx.query(point).rows == before);
                auto returned = tx.update_returning("t", {{"note", literal(std::string(1000, 'y'))}}, key);
                CHECK(returned.rows.size() == 1);
                CHECK(tx.update("t", {{"a", call("retain", {literal(std::int64_t{55})})}}, key) == 1);
                CHECK(retained->fetch(300).rows[150][1] == Value(std::int64_t{151}));
                retained.reset();
                CHECK(tx.update("t", {{"note", literal(std::string("final"))}}, key) == 1);
                CHECK(returned.rows[0][3] == Value(std::string(1000, 'y')));
                CHECK(tx.update("t", {{"a", column("a")}}, key) == 1);
                tx.integrity_check();
                committed = tx.query(point).rows;
                tx.commit();
                CHECK(old.query(point).rows[0][3] == Value(std::string("initial")));
            }
            const auto payload = db->stats().stored_payload_bytes;
            db->checkpoint();
            db.reset();
            db.emplace(Database::open(path, registry, options));
            CHECK(db->query(point).rows == committed);
            CHECK(db->stats().stored_payload_bytes == payload);
            {
                auto tx = db->begin();
                auto save = tx.savepoint();
                tx.replace("t", {std::int64_t{301}, std::int64_t{1}, std::int64_t{2}, std::string("new")});
                save.rollback();
                CHECK(tx.query(Query{"t"}).rows.size() == 300);
                tx.replace("t", {std::int64_t{301}, std::int64_t{1}, std::int64_t{2}, std::string("new")});
                tx.replace("t",
                           {std::int64_t{301}, std::int64_t{3}, std::int64_t{4}, std::string("changed")});
                tx.integrity_check();
                tx.commit();
            }
            db.reset();
            db.emplace(Database::open(path, registry, options));
            CHECK(db->query(Query{"t"}).rows.size() == 301);
            {
                auto tx = db->begin();
                tx.erase("t", Predicate{column("id"), Compare::equal, literal(std::int64_t{149})});
                tx.commit();
            }
            const auto original = db->query(Query{"t"}).rows;
            auto expected = original;
            for (auto& row : expected)
                if (row[0] == Value(std::int64_t{150}))
                    row[3] = std::string(24000, 'z');
            {
                auto retained = db->begin();
                auto tx = db->begin();
                tx.update("t", {{"note", literal(std::string(24000, 'z'))}}, key);
                CHECK(tx.query(Query{"t"}).rows == expected);
                CHECK(retained.query(Query{"t"}).rows == original);
                tx.integrity_check();
                tx.commit();
                CHECK(retained.query(Query{"t"}).rows == original);
            }
            db->checkpoint();
            db.reset();
            db.emplace(Database::open(path, registry, options));
            CHECK(db->query(Query{"t"}).rows == expected);
            db->begin().integrity_check();
        }
    });
}
