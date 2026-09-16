#include "check.hpp"
#include "../src/state.hpp"
#include "coresql/vector.hpp"
#include "coresql/timestamp.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
using namespace coresql;
int main(){return tests([]{
    Registry registry;vectors::install(registry);timestamps::install(registry);
    Database db(registry);CHECK(db.schema().empty());auto tx=db.begin();
    tx.create_table("t",{{"id",integer()},{"key",integer()},{"text",text()},{"time",timestamps::type()}});
    CHECK(tx.schema().size()==1);CHECK(db.schema().empty());
    std::mt19937 random(91);std::vector<Row> expected;
    for(std::int64_t i=0;i<500;++i){auto key=static_cast<std::int64_t>(random()%11)-5;
        Row row{i,key,std::string(static_cast<std::size_t>(random()%4),'x'),timestamps::value(key)};
        expected.push_back(row);tx.insert("t",std::move(row));}
    tx.commit();
    auto schema=db.schema();schema["t"][0].name="outside";CHECK(db.schema().at("t")[0].name=="id");
    auto snapshot=db.begin();auto newer=db.begin();newer.create_table("new",{{"id",integer()}});newer.commit();
    CHECK(!snapshot.schema().contains("new"));snapshot.rollback();expect(ErrorCode::state,[&]{snapshot.schema();});
    for(const auto& name:{"key","text","time"})for(bool descending:{false,true})for(bool filtered:{false,true}) {
        auto ordered=expected;
        if(filtered)std::erase_if(ordered,[](const Row& row){return std::get<std::int64_t>(row[1])<=0;});
        std::stable_sort(ordered.begin(),ordered.end(),[&](const Row& a,const Row& b){
            if(std::string(name)=="text")return descending?std::get<std::string>(a[2])>std::get<std::string>(b[2]):std::get<std::string>(a[2])<std::get<std::string>(b[2]);
            return descending?std::get<std::int64_t>(a[1])>std::get<std::int64_t>(b[1]):std::get<std::int64_t>(a[1])<std::get<std::int64_t>(b[1]);
        });
        for(std::size_t limit:{std::size_t{0},std::size_t{1},std::size_t{10},std::size_t{499},std::size_t{500},std::size_t{501},std::numeric_limits<std::size_t>::max()}) {
            Query query{"t",{column("id")},{},{Order{column(name),descending}},limit};
            if(filtered)query.where=Predicate{column("key"),Compare::greater,literal(std::int64_t{0})};
            detail::retained_matches() = 0;
            auto rows=db.query(query).rows;
            CHECK(detail::retained_matches() <= std::min(limit, ordered.size()));CHECK(rows.size()==std::min(limit,ordered.size()));
            for(std::size_t i=0;i<rows.size();++i)CHECK(std::get<std::int64_t>(rows[i][0])==std::get<std::int64_t>(ordered[i][0]));
        }
    }
    for(bool descending:{false,true}) {
        auto ordered=expected;
        std::stable_sort(ordered.begin(),ordered.end(),[&](const Row& a,const Row& b){
            auto first=registry.compare(a[2],b[2]);if(first)return first<0;
            auto second=registry.compare(a[3],b[3]);return descending?second>0:second<0;
        });
        for(std::size_t limit:{std::size_t{0},std::size_t{1},std::size_t{19},std::size_t{501}}) {
            Query query{"t",{column("id")},{},{Order{column("text")},Order{column("time"),descending}},limit};
            auto rows=db.query(query).rows;CHECK(rows.size()==std::min(limit,ordered.size()));
            for(std::size_t i=0;i<rows.size();++i)CHECK(rows[i][0]==ordered[i][0]);
        }
    }
    // Lazy expressions retain extension result types and computed sort keys.
    auto selected=choose({{literal(std::int64_t{1}),column("time")}},literal(Null(timestamps::type())));
    Query extension_case{"t",{column("id"),selected},{},{Order{selected},Order{column("id")}},17};
    auto plain=extension_case;plain.select[1]=column("time");plain.order_by[0].expression=column("time");
    CHECK(db.query(extension_case).rows==db.query(plain).rows);
    auto invalid=choose({{literal(std::int64_t{1}),column("time")}},literal(std::int64_t{1}));
    expect(ErrorCode::type,[&]{db.query(Query{"t",{invalid},{},{},0});});
    expect(ErrorCode::schema,[&]{evaluate_constant(choose({}),registry);});
    Query existence_input{"t",{column("time"),column("text")},{},{Order{column("time")}}};
    Query existence_outer{"t",{exists(existence_input)},{},{},1};
    detail::visited_chunks()=0;
    CHECK(db.query(existence_outer).rows==std::vector<Row>({Row{std::int64_t{1}}}));
    CHECK(detail::visited_chunks()==2); // one outer chunk and one inner chunk
    existence_input.limit=0;existence_outer.select={exists(existence_input)};
    CHECK(db.query(existence_outer).rows==std::vector<Row>({Row{std::int64_t{0}}}));
    CHECK(vectors::dimensions(vectors::type(768))==768);CHECK(!vectors::dimensions(vectors::type()));
    expect(ErrorCode::type,[&]{vectors::dimensions(integer());});
    expect(ErrorCode::format,[&]{vectors::dimensions(Type{vectors::type().id,1,Bytes(3)});});
    CHECK(registry.equal(-0.0,0.0));CHECK(registry.compare(-0.0,0.0)==0);
    CHECK(registry.compare(std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max())<0);
    CHECK(registry.compare(std::string("a\0b",3),std::string("a\0c",3))<0);
    CHECK(registry.compare(-std::numeric_limits<double>::max(),std::numeric_limits<double>::max())<0);
    expect(ErrorCode::type,[&]{registry.compare(std::numeric_limits<double>::quiet_NaN(),0.0);});
    expect(ErrorCode::type,[&]{registry.compare(0.0,std::numeric_limits<double>::infinity());});
    expect(ErrorCode::type,[&]{registry.compare(Opaque(integer(),Bytes(8)),Opaque(integer(),Bytes(8)));});
    expect(ErrorCode::type,[&]{registry.equal(std::int64_t{0},Opaque(integer(),Bytes(8)));});
    expect(ErrorCode::type,[&]{registry.equal(std::numeric_limits<double>::quiet_NaN(),0.0);});
    expect(ErrorCode::type,[&]{registry.equal(0.0,std::numeric_limits<double>::infinity());});
    expect(ErrorCode::type,[&]{registry.equal(Opaque(integer(),Bytes(8)),std::int64_t{0});});
    expect(ErrorCode::type,[&]{registry.equal(std::int64_t{1},1.0);});
    // A limited sort must still evaluate every matching row's key and report
    // errors even when an earlier valid row would satisfy the requested limit.
    auto vectors_tx=db.begin();vectors_tx.create_table("v",{{"v",vectors::type()}});
    vectors_tx.insert("v",{vectors::value(std::array{1.F,2.F})});
    vectors_tx.insert("v",{vectors::value(std::array{1.F,2.F,3.F})});vectors_tx.commit();
    Query q{"v",{}, {},{Order{call("vector.squared_l2",{column("v"),literal(vectors::value(std::array{1.F,2.F}))})}},1};
    expect(ErrorCode::type,[&]{db.query(q);});
    q.order_by.insert(q.order_by.begin(),Order{literal(std::int64_t{1})});
    expect(ErrorCode::type,[&]{db.query(q);});
    q.limit=0;CHECK(db.query(q).rows.empty());
    q.order_by={Order{column("v")}};expect(ErrorCode::unsupported,[&]{db.query(q);});
});}
