#include "check.hpp"
#include "../src/storage.hpp"
#include <algorithm>
#include <random>
using namespace coresql;
int main() { return tests([] {
    // Sparse IDs must not allocate by the largest historical ID, and replay
    // preserves their ordering and ownership across independently edited copies.
    {
        detail::State state;
        auto table = std::make_shared<detail::Table>();
        table->columns = {{"id", integer()}};
        table->next_chunk = std::numeric_limits<std::uint64_t>::max();
        for (auto id : {table->next_chunk-1, std::uint64_t{7}, std::uint64_t{1}}) {
            auto chunk = std::make_shared<detail::Chunk>();
            chunk->rows = {{static_cast<std::int64_t>(id % 100)}};
            chunk->rowids = {table->next_rowid++};
            detail::refresh(*chunk); table->chunks.emplace(id, chunk);
        }
        detail::refresh(*table); state.tables["sparse"] = table;
        auto replayed = detail::apply_changes({}, detail::encode_changes({}, state), {});
        CHECK(replayed.tables.at("sparse")->chunks.size() == 3);
        CHECK(replayed.tables.at("sparse")->chunks.begin()->first == 1);
        auto copy = table->chunks;
        CHECK(copy.erase(7) == 1); CHECK(copy.erase(7) == 0);
        CHECK(copy.contains(table->next_chunk-1)); CHECK(table->chunks.contains(7));
    }
    TempDirectory temp;
    std::optional<Database> db;
    db.emplace(Database::open(temp.path / "chunks"));
    auto t = db->begin(); t.create_table("t", {{"id",integer()},{"text",text()}});
    std::vector<std::pair<std::int64_t,std::string>> expected;
    for (std::int64_t i=0;i<1500;++i) { expected.emplace_back(i,"original"); t.insert("t",{i,std::string("original")}); }
    t.commit();
    auto old = db->begin();
    auto before = db->storage_stats().bytes_written;
    auto change = db->begin();
    CHECK(change.update("t",{{"text",literal(std::string("modified"))}},Predicate{column("id"),Compare::equal,literal(std::int64_t{128})})==1);
    change.commit(); expected[128].second="modified";
    CHECK(db->storage_stats().bytes_written-before < 8192); // One chunk, not 1500 rows.
    CHECK(std::get<std::string>(old.query(Query{"t"}).rows[128][1])=="original");
    expect(ErrorCode::conflict,[&]{old.commit();});old.rollback();
    auto verify = [&] {
        auto result=db->query(Query{"t"}); CHECK(result.rows.size()==expected.size());
        std::size_t bytes=0;
        for(std::size_t i=0;i<expected.size();++i) {
            CHECK(std::get<std::int64_t>(result.rows[i][0])==expected[i].first);
            CHECK(std::get<std::string>(result.rows[i][1])==expected[i].second);
            bytes+=8+expected[i].second.size();
        }
        CHECK(db->stats().stored_payload_bytes==bytes);
    };
    std::mt19937 rng(42);
    std::int64_t next=1500;
    for(int step=0;step<100;++step) {
        auto tx=db->begin();auto prior=expected;
        auto id=static_cast<std::int64_t>(rng()%static_cast<unsigned>(next));
        if(step%3==0) {
            std::string value(static_cast<std::size_t>(rng()%400), 'x');
            tx.update("t",{{"text",literal(value)}},Predicate{column("id"),Compare::equal,literal(id)});
            for(auto& row:expected)if(row.first==id)row.second=value;
        } else if(step%3==1) {
            tx.erase("t",Predicate{column("id"),Compare::less,literal(id/4)});
            std::erase_if(expected,[&](const auto& row){return row.first<id/4;});
        } else {tx.insert("t",{next,std::string("new")});expected.emplace_back(next++,"new");}
        if(step%7==0){tx.rollback();expected=std::move(prior);}else tx.commit();
        verify();
        if(step%11==0){db->checkpoint();verify();db.reset();db.emplace(Database::open(temp.path/"chunks"));verify();}
    }
    // Completely removed chunks stay removed across append, checkpoint and reopen.
    auto erase=db->begin();erase.erase("t");erase.commit();expected.clear();
    auto insert=db->begin();insert.insert("t",{std::int64_t{9999},std::string(20000,'z')});insert.commit();
    expected.emplace_back(9999,std::string(20000,'z'));verify();
    db->checkpoint();db.reset();db.emplace(Database::open(temp.path/"chunks"));verify();
    db->save(temp.path/"snapshot");auto imported=Database::load(temp.path/"snapshot");
    CHECK(imported.stats().stored_payload_bytes==db->stats().stored_payload_bytes);
    // A later-chunk extension failure must not publish earlier chunk edits.
    Registry registry;
    registry.add(Function{"test.late",[](std::span<const Type>){return integer();},[](std::span<const Value> v)->Value {
        auto n=std::get<std::int64_t>(v[0]);if(n==260)throw Error(ErrorCode::type,"late failure");return n+1;
    }});
    Database memory(registry);auto seed=memory.begin();seed.create_table("t",{{"id",integer()}});
    for(std::int64_t i=0;i<400;++i)seed.insert("t",{i});seed.commit();
    auto failed=memory.begin();expect(ErrorCode::type,[&]{failed.update("t",{{"id",call("test.late",{column("id")})}});});
    CHECK(std::get<std::int64_t>(failed.query(Query{"t"}).rows[128][0])==128);
    // Repeated whole-chunk deletion and insertion must not change retained
    // snapshots, leak old rows into scans, or lose rows through checkpoint/reopen.
    {
        auto path = temp.path / "churn";
        std::vector<Transaction> retained;
        auto database = Database::open(path);
        auto seed = database.begin(); seed.create_table("c", {{"id", integer(), true}, {"body", text()}}); seed.commit();
        const std::string payload(20000, 'x'); // One row per insertion chunk.
        for (std::int64_t cycle = 0; cycle < 20; ++cycle) {
            retained.push_back(database.begin());
            auto change = database.begin(); change.erase("c");
            for (std::int64_t i = 0; i < 40; ++i) change.insert("c", {cycle*40+i, payload});
            change.commit();
            CHECK(database.stats().rows == 40);
            CHECK(retained.back().query(Query{"c"}).rows.size() == (cycle ? 40 : 0));
        }
        CHECK(retained.front().query(Query{"c"}).rows.empty());
        CHECK(std::get<std::int64_t>(retained[1].query(Query{"c"}).rows[0][0]) == 0);
        database.checkpoint();
        retained.clear();
        database = Database{}; // Release the persistent owner before reopening.
        auto reopened = Database::open(path);
        auto rows = reopened.query(Query{"c"}).rows;
        CHECK(rows.size() == 40); CHECK(std::get<std::int64_t>(rows.front()[0]) == 760);
    }
}); }
