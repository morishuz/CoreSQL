#include "check.hpp"
#include "coresql/vector.hpp"
#include "coresql/timestamp.hpp"
#include "../src/state.hpp"
#include <array>
using namespace coresql;
Predicate by(std::int64_t id){return {column("id"),Compare::equal,literal(id)};}
int main(){return tests([]{
    Registry registry;vectors::install(registry);timestamps::install(registry);
    std::size_t calls=0;
    registry.add(Function{"checked",[](std::span<const Type> t){CHECK(t.size()==1&&t[0]==integer());return integer();},
        [&](std::span<const Value> v)->Value{++calls;auto i=std::get<std::int64_t>(v[0]);if(i==0)throw Error(ErrorCode::type,"zero");return i;}});
    Database db(registry);auto seed=db.begin();
    for(auto name:{"keyed","scan"}){seed.create_table(name,{{"id",integer(),std::string(name)=="keyed"},{"value",integer()}});
        for(std::int64_t i=0;i<400;++i)seed.insert(name,{i,i%7});}seed.commit();
    auto query=[&](Predicate p){return db.query(Query{"keyed",{column("id")},std::move(p),{Order{column("id")}}}).rows;};
    CHECK(query(all_of({})).size()==400);CHECK(query(any_of({})).empty());CHECK(query(not_(any_of({}))).size()==400);
    auto tree=all_of({{column("id"),Compare::greater_equal,literal(std::int64_t{10})},
        any_of({{column("id"),Compare::less_equal,literal(std::int64_t{20})},by(300)}),not_(by(15))});
    CHECK(query(tree).size()==11);
    for(auto op:{Compare::equal,Compare::not_equal,Compare::less,Compare::greater,Compare::less_equal,Compare::greater_equal}){
        auto rows=query({column("id"),op,literal(std::int64_t{200})});
        auto expected=op==Compare::equal?1:op==Compare::not_equal?399:op==Compare::less?200:op==Compare::greater?199:op==Compare::less_equal?201:200;
        CHECK(rows.size()==static_cast<std::size_t>(expected));
    }
    Predicate function{call("checked",{column("id")}),Compare::greater,literal(std::int64_t{0})};
    auto guard=all_of({all_of({by(300)}),function});
    for(auto table:{"keyed","scan"}){
        calls=0;detail::visited_chunks()=0;
        CHECK(db.query(Query{table,{},guard}).rows.size()==1);CHECK(calls==1);
        if(std::string(table)=="keyed")CHECK(detail::visited_chunks()==1);
        auto tx=db.begin();calls=0;CHECK(tx.update(table,{{"value",literal(std::int64_t{99})}},guard)==1);CHECK(calls==1);tx.rollback();
        tx=db.begin();calls=0;CHECK(tx.erase(table,guard)==1);CHECK(calls==1);tx.rollback();
        // A later key must not suppress an earlier function error on row zero.
        expect(ErrorCode::type,[&]{db.query(Query{table,{},all_of({function,by(300)})});});
        CHECK(db.query(Query{table,{},any_of({by(0),function})}).rows.size()==400);
        auto failed=db.begin();
        auto late_error=any_of({by(0),{call("checked",{literal(std::int64_t{0})}),Compare::equal,column("id")}});
        expect(ErrorCode::type,[&]{failed.erase(table,late_error);});
        expect(ErrorCode::type,[&]{failed.update(table,{{"value",literal(std::int64_t{-1})}},late_error);});
        CHECK(failed.query(Query{table}).rows.size()==400);CHECK(std::get<std::int64_t>(failed.query(Query{table,{},by(0)}).rows[0][1])==0);
    }
    calls=0;detail::visited_chunks()=0;CHECK(query(all_of({by(-1),function})).empty());CHECK(calls==0&&detail::visited_chunks()==0);
    CHECK(query(all_of({by(1),by(2)})).empty());
    detail::visited_chunks()=0;CHECK(query(any_of({by(1),by(300)})).size()==2);CHECK(detail::visited_chunks()>1);
    CHECK(query(not_(by(0))).size()==399);
    // Bind even skipped branches, missing-key queries, empty tables and LIMIT 0.
    auto empty=db.begin();empty.create_table("empty",{{"id",integer(),true}});empty.commit();
    for(auto table:{"empty","keyed"})for(auto limit:{std::size_t{0},std::size_t{10}}){
        auto bad=any_of({all_of({}),{column("missing"),Compare::equal,literal(std::int64_t{0})}});
        expect(ErrorCode::schema,[&]{db.query(Query{table,{},bad,{},limit});});
        auto mismatch=all_of({by(-1),{column("id"),Compare::equal,literal(1.0)}});
        expect(ErrorCode::type,[&]{db.query(Query{table,{},mismatch,{},limit});});
        auto tx=db.begin();expect(ErrorCode::type,[&]{tx.erase(table,mismatch);});
        expect(ErrorCode::type,[&]{tx.update(table,{{"id",literal(std::int64_t{1})}},mismatch);});
    }
    auto malformed=not_(by(1));malformed.children.clear();expect(ErrorCode::schema,[&]{query(malformed);});
    malformed=by(1);malformed.children={by(2)};expect(ErrorCode::schema,[&]{query(malformed);});
    malformed=by(1);malformed.kind=static_cast<Predicate::Kind>(99);expect(ErrorCode::schema,[&]{query(malformed);});
    malformed=by(1);malformed.operation=static_cast<Compare>(99);expect(ErrorCode::schema,[&]{query(malformed);});
    malformed=by(1);for(int i=0;i<65;++i)malformed=not_(std::move(malformed));expect(ErrorCode::schema,[&]{query(malformed);});
    expect(ErrorCode::schema,[&]{query(all_of(std::vector<Predicate>(4096,by(1))));});
    // Equality-only extensions implement != without acquiring an ordering requirement.
    auto ext=db.begin();ext.create_table("ext",{{"v",vectors::type(2)},{"time",timestamps::type()},{"s",text()},{"r",real()}});
    auto v=vectors::value(std::array{1.F,2.F},2);
    ext.insert("ext",{v,timestamps::value(10),std::string("a\0b",3),-0.0});ext.commit();
    Predicate unequal{column("v"),Compare::not_equal,literal(v)};
    CHECK(db.query(Query{"ext",{},not_(unequal)}).rows.size()==1);
    expect(ErrorCode::unsupported,[&]{db.query(Query{"ext",{},all_of({any_of({}),{column("v"),Compare::less_equal,literal(v)}})});});
    auto native=all_of({{column("s"),Compare::less_equal,literal(std::string("a\0b",3))},
        {column("r"),Compare::greater_equal,literal(0.0)},{column("time"),Compare::less_equal,literal(timestamps::value(10))}});
    CHECK(db.query(Query{"ext",{},native}).rows.size()==1);
    TempDirectory temp;db.save(temp.path/"snapshot");auto restored=Database::load(temp.path/"snapshot",registry);
    CHECK(restored.query(Query{"keyed",{},tree}).rows.size()==11);
});}
