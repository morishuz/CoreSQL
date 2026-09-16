#include "check.hpp"
#include <algorithm>
using namespace coresql;
int main(){return tests([]{
    Registry registry;
    registry.add(Function{"identity",[](std::span<const Type> t){if(t.size()!=1||t[0]!=text())throw Error(ErrorCode::type,"text required");return text();},
        [](std::span<const Value> v)->Value{return v[0];}});
    registry.add(Function{"decorate",[](std::span<const Type> t){if(t.size()!=1||t[0]!=text())throw Error(ErrorCode::type,"text required");return text();},
        [](std::span<const Value> v)->Value{return std::get<std::string>(v[0])+" suffix";}});
    registry.add(Function{"late_error",[](std::span<const Type>){return text();},[](std::span<const Value> v)->Value{
        auto id=std::get<std::int64_t>(v[0]);if(id==599)throw Error(ErrorCode::type,"late failure");return std::string(4096,'x');}});
    std::vector<Row> original;
    for(std::int64_t i=0;i<600;++i)original.push_back({i,std::to_string(i%37)+std::string(4096,'x')});
    Result survivor;
    {
        Database db(registry);auto tx=db.begin();tx.create_table("t",{{"id",integer()},{"text",text()}});
        for(const auto& row:original)tx.insert("t",row);tx.commit();
        auto target=std::get<std::string>(original[13][1]);
        // Borrowed/owned operands in both positions, including two independent
        // computed temporaries that must survive together through comparison.
        std::vector<std::optional<Predicate>> filters={std::nullopt,
            Predicate{column("text"),Compare::equal,literal(target)},
            Predicate{call("identity",{column("text")}),Compare::equal,literal(target)},
            Predicate{literal(target),Compare::equal,call("identity",{column("text")})},
            Predicate{call("identity",{literal(target)}),Compare::equal,call("identity",{column("text")})}};
        for(std::size_t f=0;f<filters.size();++f)for(int key=0;key<3;++key)for(bool descending:{false,true}) {
            auto expected=original;if(f)std::erase_if(expected,[&](const Row& row){return std::get<std::string>(row[1])!=target;});
            if(key!=1)std::stable_sort(expected.begin(),expected.end(),[&](const Row& a,const Row& b){
                return descending?std::get<std::string>(a[1])>std::get<std::string>(b[1]):std::get<std::string>(a[1])<std::get<std::string>(b[1]);});
            auto order=key==0?column("text"):key==1?literal(target):call("decorate",{column("text")});
            for(std::size_t limit:{std::size_t{1},std::size_t{17},std::size_t{1000}}) {
                auto result=db.query(Query{"t",{column("id"),column("text"),call("identity",{call("identity",{column("text")})}),literal(target)},filters[f],{Order{order,descending}},limit});
                CHECK(result.rows.size()==std::min(limit,expected.size()));
                for(std::size_t i=0;i<result.rows.size();++i){CHECK(std::get<std::int64_t>(result.rows[i][0])==std::get<std::int64_t>(expected[i][0]));
                    CHECK(std::get<std::string>(result.rows[i][1])==std::get<std::string>(expected[i][1]));
                    CHECK(std::get<std::string>(result.rows[i][2])==std::get<std::string>(expected[i][1]));CHECK(std::get<std::string>(result.rows[i][3])==target);}
            }
        }
        expect(ErrorCode::type,[&]{db.query(Query{"t",{}, {},{Order{call("late_error",{column("id")})}},1});});
        // Results own their strings even after query literals, table contents,
        // transaction snapshots and ultimately the whole database disappear.
        survivor=db.query(Query{"t",{column("text"),literal(target),call("decorate",{column("text")})},{},{Order{column("id")}},3});
        auto snapshot=db.begin();auto update=db.begin();
        CHECK(update.update("t",{{"text",literal(std::string(4096,'y'))}},Predicate{column("text"),Compare::equal,literal(target)})>0);update.commit();
        CHECK(std::get<std::string>(snapshot.query(Query{"t",{},Predicate{column("id"),Compare::equal,literal(std::int64_t{13})}}).rows[0][1])==target);snapshot.rollback();
        auto remove=db.begin();remove.erase("t",Predicate{call("identity",{column("text")}),Compare::equal,column("text")});remove.commit();CHECK(db.stats().rows==0);
    }
    std::vector<std::string> churn(1000,std::string(8192,'z'));
    for(std::size_t i=0;i<survivor.rows.size();++i){CHECK(std::get<std::string>(survivor.rows[i][0])==std::get<std::string>(original[i][1]));
        CHECK(std::get<std::string>(survivor.rows[i][1])==std::get<std::string>(original[13][1]));
        CHECK(std::get<std::string>(survivor.rows[i][2])==std::get<std::string>(original[i][1])+" suffix");}
});}
