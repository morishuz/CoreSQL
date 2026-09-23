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
        }
    });
}
