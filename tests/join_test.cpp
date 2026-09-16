#include "check.hpp"
#include "coresql/vector.hpp"
#include "../src/state.hpp"
#include "../src/storage.hpp"
#include <array>
#include <cstring>
#include <sys/wait.h>
using namespace coresql;
Query joined(std::string right="collections") {
    Query q{"documents"};q.alias="d";
    q.join=InnerJoin{std::move(right),"c",column("d","collection"),column("c","id")};return q;
}
namespace {const char* stop=nullptr;void crash(const char* stage){if(std::strcmp(stage,stop)==0)::_exit(77);}}
int main(){return tests([]{
    Registry registry;vectors::install(registry);
    registry.add(Function{"bad",[](std::span<const Type>){return integer();},[](std::span<const Value>)->Value{throw Error(ErrorCode::type,"test failure");}});
    Database db(registry);auto seed=db.begin();
    seed.create_table("documents",{{"id",integer(),true},{"collection",integer()},{"title",text()}});
    for(std::int64_t i=0;i<300;++i)seed.insert("documents",{i,i%5,std::string("doc")+std::to_string(i)});
    for(auto name:{"collections","unkeyed"}){
        seed.create_table(
            name,
            {{"id", integer(), std::string(name) == "collections", {}, {}, std::string(name) == "unkeyed"},
             {"name", text()}});
        for(std::int64_t i=0;i<4;++i)seed.insert(name,{i,std::string(i%2?"beta":"alpha")});
    }
    seed.create_table("empty",{{"id",integer(),true},{"name",text()}});seed.commit();
    auto q=joined();auto all=db.query(q);CHECK(all.rows.size()==240&&all.types.size()==5);
    CHECK(std::get<std::int64_t>(all.rows[0][0])==0);CHECK(std::get<std::string>(all.rows[0][4])=="alpha");
    for(bool descending:{false,true})for(std::size_t limit:{std::size_t{0},std::size_t{1},std::size_t{10},std::size_t{500}}){
        auto a=joined(),b=joined("unkeyed");
        a.select=b.select={column("d","id"),column("c","name")};
        a.order_by=b.order_by={Order{column("c","name"),descending}};a.limit=b.limit=limit;
        a.where=b.where=all_of({{column("d","id"),Compare::greater_equal,literal(std::int64_t{10})},not_({column("c","id"),Compare::equal,literal(std::int64_t{2})})});
        detail::retained_matches()=0;auto indexed=db.query(a);CHECK(detail::retained_matches()<=limit);auto scanned=db.query(b);
        CHECK(indexed.rows.size()==scanned.rows.size());
        for(std::size_t i=0;i<indexed.rows.size();++i){CHECK(std::get<std::int64_t>(indexed.rows[i][0])==std::get<std::int64_t>(scanned.rows[i][0]));CHECK(std::get<std::string>(indexed.rows[i][1])==std::get<std::string>(scanned.rows[i][1]));}
    }
    detail::QueryCounters indexed_counts, hash_counts;
    {
        detail::QueryCounterScope scope(indexed_counts);
        db.query(joined());
    }
    {
        detail::QueryCounterScope scope(hash_counts);
        db.query(joined("unkeyed"));
    }
    CHECK(indexed_counts.hash_build_rows == 0); // Existing indexes take precedence.
    CHECK(indexed_counts.candidate_pairs == 240);
    CHECK(hash_counts.hash_build_rows == 4 && hash_counts.hash_probes == 300);
    CHECK(hash_counts.candidate_pairs == 240);
    q=joined();std::swap(q.join->left,q.join->right);CHECK(db.query(q).rows.size()==240);
    q=joined("empty");CHECK(db.query(q).rows.empty());
    auto expect_schema=[&](Query invalid){invalid.limit=0;expect(ErrorCode::schema,[&]{db.query(invalid);});};
    q=joined();q.alias="";expect_schema(q);q=joined();q.join->alias="d";expect_schema(q);
    q=joined();q.select={column("id")};expect_schema(q);q=joined();q.select={column("x","id")};expect_schema(q);
    q=joined();q.join->right=column("d","id");expect_schema(q);q=joined();q.join->right=literal(std::int64_t{1});expect_schema(q);
    q=joined();q.join->table="missing";expect_schema(q);
    q=joined();q.join->right=column("c","name");expect(ErrorCode::type,[&]{db.query(q);});
    q=joined("empty");q.where=Predicate{column("c","missing"),Compare::equal,literal(std::int64_t{1})};expect_schema(q);
    // Self joins and one-to-many matches retain deterministic left/right scan order.
    Query self{"documents"};self.alias="a";self.join=InnerJoin{"documents","b",column("a","collection"),column("b","collection")};
    self.select={column("a","id"),column("b","id")};self.limit=3;
    auto pairs=db.query(self).rows;CHECK(pairs.size()==3);CHECK(std::get<std::int64_t>(pairs[2][0])==0&&std::get<std::int64_t>(pairs[2][1])==10);
    self.limit=std::numeric_limits<std::size_t>::max();CHECK(db.query(self).rows.size()==18000);
    // WHERE is evaluated only for ON matches; an empty inner side suppresses runtime errors.
    q=joined("empty");q.where=Predicate{call("bad",{}),Compare::equal,literal(std::int64_t{1})};CHECK(db.query(q).rows.empty());
    q.join->table="collections";expect(ErrorCode::type,[&]{db.query(q);});
    q.where.reset();q.order_by={Order{call("bad",{})}};q.limit=1;expect(ErrorCode::type,[&]{db.query(q);});
    q = joined("unkeyed");
    q.where = Predicate{call("bad", {}), Compare::equal, literal(std::int64_t{1})};
    expect(ErrorCode::type, [&] { db.query(q); });
    CHECK(db.query(joined("unkeyed")).rows.size() == 240);
    // Opaque equality joins use extension equality without requiring an index or ordering.
    auto ext=db.begin();ext.create_table("vectors",{{"v",vectors::type(2)}});
    ext.insert("vectors",{vectors::value(std::array{1.F,2.F},2)});ext.insert("vectors",{vectors::value(std::array{3.F,4.F},2)});ext.commit();
    Query v{"vectors"};v.alias="a";v.join=InnerJoin{"vectors","b",column("a","v"),column("b","v")};CHECK(db.query(v).rows.size()==2);
    v.order_by={Order{column("b","v")}};v.limit=0;expect(ErrorCode::unsupported,[&]{db.query(v);});
    auto old=db.begin(),clear=db.begin();detail::visited_chunks()=0;
    CHECK(clear.erase("collections")==4);CHECK(detail::visited_chunks()==0);CHECK(clear.query(joined()).rows.empty());
    CHECK(old.query(joined()).rows.size()==240);clear.rollback();CHECK(db.query(joined()).rows.size()==240);
    clear=db.begin();CHECK(clear.erase("collections")==4);clear.insert("collections",{std::int64_t{0},std::string("new")});clear.commit();
    CHECK(db.query(joined()).rows.size()==60);CHECK(old.query(joined()).rows.size()==240);
    expect(ErrorCode::conflict,[&]{old.commit();});old.rollback();
    // Hash buckets preserve right-side duplicate order, ignore NULL keys and
    // belong to one query snapshot rather than a reusable global cache.
    auto prior = db.begin();
    auto add = db.begin();
    add.insert("unkeyed", {Null(integer()), std::string("null")});
    add.insert("unkeyed", {std::int64_t{0}, std::string("duplicate")});
    add.create_table("null_keys", {{"id", integer(), false, {}, {}, true}});
    add.insert("null_keys", {Null(integer())});
    add.create_table("extreme_keys", {{"id", integer()}});
    for (auto key : {std::numeric_limits<std::int64_t>::min(), std::int64_t{0},
                     std::numeric_limits<std::int64_t>::max()})
        add.insert("extreme_keys", {key});
    add.commit();
    CHECK(prior.query(joined("unkeyed")).rows.size() == 240);
    auto duplicates = db.query(joined("unkeyed"));
    CHECK(duplicates.rows.size() == 300);
    CHECK(std::get<std::string>(duplicates.rows[0][4]) == "alpha");
    CHECK(std::get<std::string>(duplicates.rows[1][4]) == "duplicate");
    auto limited = joined("unkeyed");
    limited.limit = 1;
    detail::QueryCounters limited_counts;
    {
        detail::QueryCounterScope scope(limited_counts);
        CHECK(db.query(limited).rows.size() == 1);
    }
    CHECK(limited_counts.hash_build_rows == 6 && limited_counts.hash_probes == 1);
    CHECK(limited_counts.candidate_pairs == 1);
    limited.limit = 0;
    detail::QueryCounters no_work;
    {
        detail::QueryCounterScope scope(no_work);
        CHECK(db.query(limited).rows.empty());
    }
    Query null_left{"null_keys"};
    null_left.alias = "n";
    null_left.join = InnerJoin{"unkeyed", "u", column("n", "id"), column("u", "id")};
    {
        detail::QueryCounterScope scope(no_work);
        CHECK(db.query(null_left).rows.empty());
    }
    null_left.where = Predicate{call("bad", {}), Compare::equal, literal(std::int64_t{1})};
    {
        detail::QueryCounterScope scope(no_work);
        CHECK(db.query(null_left).rows.empty());
    }
    null_left.table = "empty";
    {
        detail::QueryCounterScope scope(no_work);
        CHECK(db.query(null_left).rows.empty());
    }
    CHECK(no_work.hash_build_rows == 0 && no_work.hash_probes == 0);
    Query extremes{"extreme_keys"};
    extremes.alias = "a";
    extremes.join = InnerJoin{"extreme_keys", "b", column("a", "id"), column("b", "id")};
    CHECK(db.query(extremes).rows.size() == 3);
    auto remove = db.begin();
    remove.erase("unkeyed");
    remove.commit();
    CHECK(db.query(joined("unkeyed")).rows.empty());
    CHECK(prior.query(joined("unkeyed")).rows.size() == 240);
    prior.rollback();
    // Sparse historical IDs, snapshots, clear/reinsert, checkpoint and log recovery.
    TempDirectory temp;auto path=temp.path/"live";
    {auto disk=Database::open(path);auto tx=disk.begin();tx.create_table("t",{{"id",integer(),true}});
     for(std::int64_t i=0;i<300;++i)tx.insert("t",{i});tx.commit();}
    for(auto stage:{"half_payload","footer"}){
        auto file=temp.path/stage;std::filesystem::copy_file(path,file);auto pid=::fork();CHECK(pid>=0);
        if(pid==0){try{auto disk=Database::open(file);auto tx=disk.begin();tx.erase("t");stop=stage;detail::storage_hook(crash);tx.commit();::_exit(78);}catch(...){::_exit(79);}}
        int status;CHECK(::waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==77);
        auto recovered=Database::open(file);CHECK(recovered.stats().rows==(std::strcmp(stage,"footer")==0?0:300));
    }
    {auto disk=Database::open(path);auto before=disk.begin(),tx=disk.begin();CHECK(tx.erase("t")==300);
     tx.insert("t",{std::int64_t{1}});tx.commit();CHECK(before.query(Query{"t"}).rows.size()==300);disk.checkpoint();}
    {auto disk=Database::open(path);CHECK(disk.stats().rows==1);auto tx=disk.begin();expect(ErrorCode::constraint,[&]{tx.insert("t",{std::int64_t{1}});});}
    db.save(temp.path/"export");auto loaded=Database::load(temp.path/"export",registry);CHECK(loaded.query(joined()).rows.size()==60);
    auto owned=loaded.query(joined());CHECK(std::get<std::string>(owned.rows[0][4])=="new");
});}
