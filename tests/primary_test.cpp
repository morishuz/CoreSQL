#include "check.hpp"
#include "coresql/vector.hpp"
#include "coresql/encoding.hpp"
#include "../src/storage.hpp"
#include "../src/index.hpp"
#include <random>
using namespace coresql;
Predicate by(std::int64_t id) { return {column("id"),Compare::equal,literal(id)}; }
Query get(std::int64_t id) { return {"items",{},by(id)}; }
int main() { return tests([] {
    Registry registry; vectors::install(registry);
    registry.add(Function{"negate",[](std::span<const Type> args) {
        CHECK(args.size()==1 && args[0]==integer());return integer();
    },[](std::span<const Value> args)->Value{return -std::get<std::int64_t>(args[0]);}});
    registry.add(Function{"fail",[](std::span<const Type>){return integer();},
        [](std::span<const Value> args)->Value{
            if(std::get<std::int64_t>(args[0])==200)throw Error(ErrorCode::type,"test failure");
            return std::get<std::int64_t>(args[0])+1000;
        }});
    Database db(registry);
    auto t=db.begin();
    expect(ErrorCode::schema,[&]{t.create_table("bad",{{"a",integer(),true},{"b",integer(),true}});});
    expect(ErrorCode::unsupported,[&]{t.create_table("bad",{{"a",vectors::type(),true}});});
    t.create_table("items",{{"id",integer(),true},{"value",integer()}});
    for(std::int64_t i=0;i<600;++i)t.insert("items",{i,i});
    expect(ErrorCode::constraint,[&]{t.insert("items",{std::int64_t{0},std::int64_t{99}});});
    t.commit();CHECK(db.stats().rows==600);CHECK(db.schema().at("items")[0].primary_key);
    // Failed-only and no-match transactions publish nothing: an existing writer
    // can still commit. A failed write also leaves the transaction reusable.
    {
        auto witness = db.begin();
        auto failed = db.begin();
        expect(ErrorCode::constraint, [&] { failed.insert("items", {std::int64_t{0}, std::int64_t{99}}); });
        expect(ErrorCode::constraint, [&] { failed.update("items", {{"id", literal(std::int64_t{1})}}, by(0)); });
        failed.commit();
        auto miss = db.begin();
        CHECK(miss.update("items", {{"value", literal(std::int64_t{9})}}, by(-1)) == 0);
        CHECK(miss.erase("items", by(-1)) == 0);
        miss.commit();
        witness.insert("items", {std::int64_t{900}, std::int64_t{9}});
        witness.commit();
        auto reuse = db.begin();
        expect(ErrorCode::constraint, [&] { reuse.insert("items", {std::int64_t{900}, std::int64_t{0}}); });
        expect(ErrorCode::constraint, [&] { reuse.update("items", {{"id", literal(std::int64_t{1})}}, by(900)); });
        CHECK(reuse.erase("items", by(900)) == 1);
        reuse.commit();
        CHECK(db.stats().rows == 600);
    }
    // Verify actual traversal, including reversed equality and range candidates.
    {
        detail::visited_chunks() = 0;
        CHECK(db.query(get(256)).rows.size() == 1);
        CHECK(detail::visited_chunks() == 1);
        detail::visited_chunks() = 0;
        CHECK(db.query(get(-1)).rows.empty());
        CHECK(detail::visited_chunks() == 0);
        auto access = db.begin();
        Predicate reversed{literal(std::int64_t{256}), Compare::equal, column("id")};
        detail::visited_chunks() = 0;
        CHECK(access.update("items", {{"value", literal(std::int64_t{9})}}, reversed) == 1);
        CHECK(detail::visited_chunks() == 1);
        detail::visited_chunks() = 0;
        CHECK(access.erase("items", by(-1)) == 0);
        CHECK(detail::visited_chunks() == 0);
        detail::visited_chunks() = 0;
        CHECK(access.erase("items", reversed) == 1);
        CHECK(detail::visited_chunks() == 1);
        access.rollback();
        detail::visited_chunks() = 0;
        CHECK(db.query(Query{"items", {}, Predicate{column("id"), Compare::less, literal(std::int64_t{0})}}).rows.empty());
        CHECK(detail::visited_chunks() <= 1); // At most the inclusive boundary candidate.
    }
    auto old=db.begin(), write=db.begin();
    CHECK(write.update("items",{{"id",literal(std::int64_t{700})}},by(256))==1);
    CHECK(write.query(get(256)).rows.empty());CHECK(write.query(get(700)).rows.size()==1);
    CHECK(old.query(get(256)).rows.size()==1);CHECK(old.query(get(700)).rows.empty());
    expect(ErrorCode::constraint,[&]{write.update("items",{{"id",literal(std::int64_t{257})}},by(700));});
    CHECK(write.query(get(700)).rows.size()==1);
    expect(ErrorCode::type,[&]{write.update("items",{{"id",call("fail",{column("id")})}});});
    CHECK(write.query(get(0)).rows.size()==1);CHECK(write.query(get(700)).rows.size()==1);
    write.commit();old.insert("items",{std::int64_t{700},std::int64_t{7}});
    expect(ErrorCode::conflict,[&]{old.commit();});old.rollback();
    // Deletions shift row offsets within chunks. Every surviving key still works.
    auto del=db.begin();CHECK(del.erase("items",by(1))==1);CHECK(del.erase("items",by(700))==1);
    del.insert("items",{std::int64_t{1},std::int64_t{999}});
    for(std::int64_t i=0;i<600;++i)CHECK(del.query(get(i)).rows.size()==(i==256?0:1));
    del.rollback();CHECK(db.query(get(700)).rows.size()==1);
    auto swap=db.begin();swap.create_table("swap",{{"id",integer(),true}});
    for(std::int64_t i=-300;i<=300;++i)swap.insert("swap",{i});
    CHECK(swap.update("swap",{{"id",call("negate",{column("id")})}})==601);
    CHECK(std::get<std::int64_t>(swap.query(Query{"swap"}).rows[0][0])==300);swap.commit();
    auto native=db.begin();native.create_table("real",{{"key",real(),true}});
    native.insert("real",{-0.0});expect(ErrorCode::constraint,[&]{native.insert("real",{0.0});});
    native.create_table("text",{{"payload",integer()},{"key",text(),true}});
    for(auto s:{std::string(""),std::string("a"),std::string("a\0b",3)})native.insert("text",{std::int64_t{1},s});
    expect(ErrorCode::constraint,[&]{native.insert("text",{std::int64_t{2},std::string("a\0b",3)});});
    native.commit();
    CHECK(db.query(Query{"real",{},Predicate{literal(0.0),Compare::equal,column("key")}}).rows.size()==1);
    CHECK(db.query(Query{"text",{},Predicate{column("key"),Compare::equal,literal(std::string("a\0b",3))}}).rows.size()==1);
    CHECK(db.query(Query{"items",{},Predicate{literal(std::int64_t{10}),Compare::equal,column("id")}}).rows.size()==1);
    CHECK(db.query(Query{"items",{},Predicate{call("negate",{column("id")}),Compare::equal,literal(std::int64_t{-10})}}).rows.size()==1);
    CHECK(db.query(Query{"items",{},Predicate{column("id"),Compare::less,literal(std::int64_t{10})}}).rows.size()==10);
    CHECK(db.query(Query{"items",{},by(-1),{Order{column("value")}}}).rows.empty());
    expect(ErrorCode::schema,[&]{db.query(Query{"items",{column("absent")},by(-1),{},0});});
    expect(ErrorCode::type,[&]{db.query(Query{"items",{},Predicate{column("id"),Compare::equal,literal(1.0)}});});
    auto missing=db.begin();expect(ErrorCode::schema,[&]{missing.update("items",{{"absent",literal(std::int64_t{0})}},by(-1));});
    CHECK(missing.update("items",{{"value",literal(std::int64_t{0})}},by(-1))==0);CHECK(missing.erase("items",by(-1))==0);missing.rollback();
    TempDirectory temp;db.save(temp.path/"snapshot");auto loaded=Database::load(temp.path/"snapshot",registry);
    CHECK(loaded.schema()==db.schema());auto duplicate=loaded.begin();
    expect(ErrorCode::constraint,[&]{duplicate.insert("items",{std::int64_t{700},std::int64_t{0}});});
    // Random mutations, failed statements, rollbacks, checkpoints and reopenings
    // against a separate key/value model; exercise more than one chunk/bucket.
    std::map<std::int64_t,std::int64_t> model;
    std::optional<Database> disk;disk.emplace(Database::open(temp.path/"live"));
    {auto seed=disk->begin();seed.create_table("items",{{"id",integer(),true},{"value",integer()}});
     for(std::int64_t i=0;i<600;++i){seed.insert("items",{i,i});model[i]=i;}seed.commit();}
    std::mt19937 random(123);
    for(int step=0;step<200;++step){
        auto expected=model;auto tx=disk->begin();
        for(int j=0;j<5;++j){std::int64_t id=random()%800,value=random()%800;
            switch(random()%4){
            case 0:
                if(expected.contains(id))expect(ErrorCode::constraint,[&]{tx.insert("items",{id,value});});
                else {tx.insert("items",{id,value});expected[id]=value;}break;
            case 1: CHECK(tx.erase("items",by(id))==expected.erase(id));break;
            case 2: CHECK(tx.update("items",{{"value",literal(value)}},by(id))==expected.count(id));if(expected.contains(id))expected[id]=value;break;
            case 3:
                if(expected.contains(id)&&expected.contains(value)&&id!=value)
                    expect(ErrorCode::constraint,[&]{tx.update("items",{{"id",literal(value)}},by(id));});
                else {CHECK(tx.update("items",{{"id",literal(value)}},by(id))==expected.count(id));
                    if(expected.contains(id)){auto data=expected.at(id);expected.erase(id);expected[value]=data;}}break;
            }
        }
        if(step%7==0)tx.rollback();else{tx.commit();model=expected;}
        if(step%10==0){if(step%20==0)disk->checkpoint();disk.reset();disk.emplace(Database::open(temp.path/"live"));}
        CHECK(disk->stats().rows==model.size());
        for(std::int64_t id=0;id<800;++id){auto rows=disk->query(get(id)).rows;CHECK(rows.size()==model.count(id));
            if(!rows.empty())CHECK(std::get<std::int64_t>(rows[0][1])==model.at(id));}
    }
    // Independent legacy encodings: old unkeyed files remain readable. New
    // changes retain that schema, rather than silently adding constraints.
    auto legacy=[&](bool change, bool keyed = false) {
        Bytes bytes;
        auto field=[&](std::string_view str){encoding::u64(bytes,str.size());
            auto view=std::as_bytes(std::span(str.data(),str.size()));bytes.insert(bytes.end(),view.begin(),view.end());};
        field(change ? (keyed ? "CORECHG3" : "CORECHG2") : (keyed ? "CORESQL2" : "CORESQL1"));encoding::u64(bytes,1);
        field("old");encoding::u64(bytes,1);field("id");field("core.integer");encoding::u64(bytes,1);field("");
        if(keyed) encoding::u64(bytes,1);
        if(change){encoding::u64(bytes,1);encoding::u64(bytes,1);encoding::u64(bytes,0);}
        encoding::u64(bytes,2);encoding::u64(bytes,7);encoding::u64(bytes,keyed ? 8 : 7);
        std::uint64_t hash=14695981039346656037ULL;
        for(auto byte:bytes){hash^=std::to_integer<unsigned>(byte);hash*=1099511628211ULL;}encoding::u64(bytes,hash);
        return bytes;
    };
    {auto bytes=legacy(false);std::ofstream file(temp.path/"legacy",std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    auto imported=Database::load(temp.path/"legacy");CHECK(imported.stats().rows==2);
    CHECK(!imported.schema().at("old")[0].primary_key);
    auto replayed=detail::apply_changes({},legacy(true),{});detail::rebuild_indexes(replayed, {});
    CHECK(!replayed.tables.at("old")->primary);CHECK(replayed.stats.rows==2);
    {auto bytes=legacy(false,true);std::ofstream file(temp.path/"legacy-keyed",std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    auto keyed_import=Database::load(temp.path/"legacy-keyed");
    CHECK(keyed_import.schema().at("old")[0].primary_key);
    auto keyed_replay=detail::apply_changes({},legacy(true,true),{}); detail::rebuild_indexes(keyed_replay,{});
    CHECK(keyed_replay.tables.at("old")->primary != nullptr);
    auto rewritten=detail::apply_changes(replayed,detail::encode_changes({},replayed),{});
    CHECK(rewritten.stats.rows==2);
    // Decoder rejects duplicate keys even when a record has a valid checksum.
    detail::State invalid;auto table=std::make_shared<detail::Table>();table->columns={{"id",integer(),true}};
    auto chunk=std::make_shared<detail::Chunk>();chunk->rows={{std::int64_t{1}},{std::int64_t{1}}};chunk->rowids={1,2};table->next_rowid=3;detail::refresh(*chunk);
    table->chunks[0]=chunk;table->next_chunk=1;detail::refresh(*table);invalid.tables["bad"]=table;
    auto decoded=detail::apply_changes({},detail::encode_changes({},invalid),{});
    expect(ErrorCode::constraint,[&]{detail::rebuild_indexes(decoded, {});});
}); }
