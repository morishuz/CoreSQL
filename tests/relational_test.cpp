#include "check.hpp"
#include "../src/ordered_index.hpp"
#include <random>
#include <algorithm>
using namespace coresql;
Value n(std::int64_t i) { return i; }
Predicate eq(std::string c,Value v) { return {column(c),Compare::equal,literal(v)}; }
Predicate range(std::string c,Value lo,Value hi) { return all_of({{column(c),Compare::greater_equal,literal(lo)},{column(c),Compare::less_equal,literal(hi)}}); }
std::int64_t integer_result(const Result& r,std::size_t i=0) { return std::get<std::int64_t>(r.rows[0][i]); }
int main(){return tests([]{
    TempDirectory temp;
    auto db=Database::open(temp.path/"db");
    auto tx=db.begin();tx.create_table("t",{{"id",integer()},{"s",text()}});
    tx.create_index("t",{"id",{"id"},true});tx.create_index("t",{"s",{"s"},false,{true}});
    tx.insert("t",{n(1),std::string("one")});tx.insert("t",{n(2),std::string("two")});tx.insert("t",{n(3),std::string("two")});tx.commit();
    auto old=db.begin();
    tx=db.begin();
    auto q=Query{"t",{aggregate("count"),aggregate("sum",{column("id")}),aggregate("avg",{column("id")}),aggregate("group_concat",{column("s")})}};
    auto result=tx.query(q);CHECK(integer_result(result)==3);CHECK(integer_result(result,1)==6);CHECK(std::get<double>(result.rows[0][2])==2);CHECK(std::get<std::string>(result.rows[0][3])=="one,two,two");
    q.where=eq("id",n(20));result=tx.query(q);CHECK(integer_result(result)==0);CHECK(is_null(result.rows[0][1])&&is_null(result.rows[0][2])&&is_null(result.rows[0][3]));
    q.limit=0;CHECK(tx.query(q).rows.empty());q.table="absent";expect(ErrorCode::schema,[&]{tx.query(q);});
    expect(ErrorCode::schema,[&]{tx.query({"t",{aggregate("sum",{aggregate("count")})}});});
    Query grouped{"t",{column("s"),aggregate("count")}};grouped.group_by={column("s")};grouped.order_by={{column("s")}};
    CHECK(tx.query(grouped).rows==std::vector<Row>({{std::string("one"),n(1)},{std::string("two"),n(2)}}));
    Query combined{"t",{column("s")}};combined.compounds={{SetOperation::intersect,std::make_shared<const Query>(combined)}};
    CHECK(tx.query(combined).rows.size()==2);
    Query crossed{"t",{column("a","id"),column("b","id")}};crossed.alias="a";
    crossed.joins={{"t","b",literal(n(0)),literal(n(0)),true}};crossed.order_by={{column("b","id"),true},{column("a","id"),true}};crossed.limit=2;
    CHECK(tx.query(crossed).rows==std::vector<Row>({{n(3),n(3)},{n(2),n(3)}}));
    Predicate unknown{column("id"),Compare::equal,literal(Null(integer()))};
    CHECK(tx.query({"t",{},not_(unknown)}).rows.empty());
    CHECK(tx.query({"t",{},any_of({unknown,eq("id",n(2))})}).rows.size()==1);
    CHECK(tx.query({"t",{},not_(all_of({unknown,eq("id",n(99))}))}).rows.size()==3);
    expect(ErrorCode::constraint,[&]{tx.insert("t",{Null(integer()),std::string("x")});});
    Query distinct{"t",{column("s")}};distinct.distinct=true;CHECK(tx.query(distinct).rows.size()==2);
    auto before=tx.query({"t",{}}).rows;
    expect(ErrorCode::constraint,[&]{tx.create_index("t",{"bad",{"s"},true});});
    expect(ErrorCode::constraint,[&]{tx.insert("t",{n(1),std::string("bad")});});
    expect(ErrorCode::constraint,[&]{tx.update("t",{{"id",literal(n(1))}});});
    CHECK(tx.query({"t",{}}).rows==before);
    tx.add_column("t",{"d",integer(),false,{},n(123)});
    tx.replace("t",{n(2),std::string("new")});
    CHECK(tx.query({"t",{},eq("id",n(2))}).rows[0][2]==n(123));
    // Both row writers validate the whole row before changing unique indexes.
    before = tx.query({"t", {}}).rows;
    for (bool replacing : {false, true}) {
        auto write = [&](Row row) {
            if (replacing) tx.replace("t", std::move(row));
            else tx.insert("t", std::move(row));
        };
        expect(ErrorCode::schema, [&] { write({n(2)}); });
        expect(ErrorCode::type, [&] { write({n(2), n(9)}); });
        expect(ErrorCode::constraint, [&] { write({n(2), Null(text())}); });
        CHECK(tx.query({"t", {}}).rows == before);
        tx.integrity_check();
    }
    CHECK(tx.query({"t",{},range("s",std::string("a"),std::string("p"))}).rows.size()==2);
    tx.create_table("input",{{"id",integer()},{"s",text()}});tx.insert("input",{n(4),std::string("four")});tx.insert("input",{n(1),std::string("duplicate")});
    expect(ErrorCode::constraint,[&]{tx.insert_from("t",{"input",{}});});
    expect(ErrorCode::schema,[&]{tx.insert_from("missing",{"input",{},eq("id",n(100))});});
    expect(ErrorCode::type,[&]{tx.insert_from("t",{"input",{column("s"),column("id")},eq("id",n(100))});});
    CHECK(tx.query({"t",{},eq("id",n(4))}).rows.empty());
    tx.commit();CHECK(old.schema().at("t").size()==2);CHECK(old.query({"t",{},eq("id",n(2))}).rows[0][1]==Value(std::string("two")));old.rollback();
    auto verify=[&](Database& db){auto t=db.begin();t.integrity_check();CHECK(t.query({"t",{},range("id",n(1),n(3))}).rows.size()==3);expect(ErrorCode::constraint,[&]{t.insert("t",{n(1),std::string("duplicate")});});t.insert("t",{n(8),std::string("eight")});CHECK(t.query({"t",{},eq("id",n(8))}).rows[0][2]==n(123));};
    verify(db);
    auto saved_ids=db.query({"t",{row_id()}, {},{Order{column("id")}}}).rows;
    tx=db.begin();tx.vacuum();tx.integrity_check();CHECK(tx.query({"t",{row_id()}, {},{Order{column("id")}}}).rows==saved_ids);tx.commit();
    db.save(temp.path/"snapshot");auto imported=Database::load(temp.path/"snapshot");verify(imported);CHECK(imported.query({"t",{row_id()}, {},{Order{column("id")}}}).rows==saved_ids);
    db=Database();db=Database::open(temp.path/"db");verify(db);CHECK(db.query({"t",{row_id()}, {},{Order{column("id")}}}).rows==saved_ids);db.checkpoint();db=Database();db=Database::open(temp.path/"db");verify(db);
    // Scalar subqueries bind correlated parameters, return typed NULL on no match,
    // and do not cache results from a different outer row.
    Query sub{"t",{column("d")},eq("id",n(1))};sub.where=Predicate{column("id"),Compare::equal,parameter("key")};
    auto scalar=scalar_subquery(sub,{{"key",column("id")}});
    auto correlated=db.query({"input",{scalar}, {},{Order{column("id")}}});
    CHECK(correlated.rows.size()==2);CHECK(correlated.rows[0][0]==n(123));CHECK(is_null(correlated.rows[1][0]));
    // IN's needle must not shift either correlated capture; all three query
    // forms use the same substitutions and preserve empty-result semantics.
    sub.where = all_of({
        {column("id"), Compare::equal, parameter("key")},
        {column("id"), Compare::greater_equal, parameter("minimum")}
    });
    std::vector<std::pair<std::string, Expr>> captures{
        {"key", column("id")}, {"minimum", literal(n(1))}
    };
    Registry capture_registry;
    capture_registry.add(Function{"test.equal", [](std::span<const Type> types) {
        CHECK(types.size() == 2 && types[0] == integer() && types[1] == integer());
        return integer();
    }, [](std::span<const Value> values) -> Value { return n(values[0] == values[1]); }});
    auto captured_db = Database::load(temp.path/"snapshot", capture_registry);
    Query captured{"input", {
        scalar_subquery(sub, captures), exists(sub, captures),
        membership(literal(n(123)), sub, "test.equal", captures)
    }, {}, {Order{column("id")}}};
    CHECK(captured_db.query(captured).rows == std::vector<Row>({
        {n(123), n(1), n(1)}, {Null(integer()), n(0), n(0)}
    }));
    Query joins{"input",{column("c","s")}};joins.alias="a";joins.joins={{"t","b",column("a","id"),column("b","id")},{"t","c",column("b","id"),column("c","id")}};
    CHECK(db.query(joins).rows==std::vector<Row>{{std::string("one")}});
    joins.limit=0;CHECK(db.query(joins).rows.empty());
    joins.joins.back().alias="left";joins.select={column("left","s")};joins.joins.back().right=column("left","id");
    CHECK(db.query(joins).rows.empty());joins.limit=10;CHECK(db.query(joins).rows.size()==1);
    Query relation_query{"local", {aggregate("sum", {column("value")})}};
    relation_query.relations = {
        {"local", std::make_shared<const Query>(Query{"t", {column("id")}}), {{"value", integer()}}}};
    CHECK(integer_result(db.query(relation_query)) == 6);
    {
        auto snapshot = db.begin();
        auto writer = db.begin();
        writer.insert("t", {n(4), std::string("four")});
        writer.commit();
        CHECK(integer_result(snapshot.query(relation_query)) == 6);
        CHECK(integer_result(db.query(relation_query)) == 10);
    }
    auto bad_relation = relation_query;
    bad_relation.relations[0].columns[0].type = text();
    expect(ErrorCode::schema, [&] { db.query(bad_relation); });
    bad_relation = relation_query;
    bad_relation.relations[0].query = std::make_shared<const Query>(Query{"local", {column("value")}});
    expect(ErrorCode::unsupported, [&] { db.query(bad_relation); });
    // Randomized ordered-index insert/delete/range checks, including descending
    // order, duplicate leading keys, and immutable clones.
    for(bool descending:{false,true}) {
        Registry r;detail::OrderedIndex index({"x",{"a","b"},false,{descending,false}},{{"a",integer()},{"b",integer()}},r);
        std::map<unsigned,Row> rows;std::mt19937 rng(419);
        for(unsigned i=0;i<350;++i) {
            if(i%3==0&&!rows.empty()){auto p=rows.begin();std::advance(p,rng()%rows.size());index.erase(p->second,{0,p->first});rows.erase(p);}
            else {Row row{n(rng()%20),n(rng()%30)};index.insert(row,{0,i});rows.emplace(i,row);}
            CHECK(index.validate()==rows.size());
            auto copy=index;Row extra{n(100),n(100)};copy.insert(extra,{1,i});CHECK(index.range(n(100),n(100)).empty());
            unsigned lo=rng()%20,hi=lo+rng()%10;auto found=index.range(n(lo),n(hi));std::sort(found.begin(),found.end());std::vector<RowLocation> expected;
            for(const auto& [id,row]:rows)if(std::get<std::int64_t>(row[0])>=lo&&std::get<std::int64_t>(row[0])<=hi)expected.push_back({0,id});CHECK(found==expected);
        }
    }
});}
