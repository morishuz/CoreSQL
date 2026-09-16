#include "check.hpp"
#include "coresql/vector.hpp"
#include "coresql/timestamp.hpp"
#include <array>
using namespace coresql;
int main(){return tests([]{
 Registry r; vectors::install(r);timestamps::install(r);
 r.add(Function{"fail_late",[](std::span<const Type> a){if(a.size()!=1||a[0]!=integer())throw Error(ErrorCode::type,"arg");return integer();},[](std::span<const Value> a)->Value{auto n=std::get<std::int64_t>(a[0]);if(n==2)throw Error(ErrorCode::type,"late failure");return n+10;}});
 Database db(r);auto t=db.begin();
 t.create_table("t",{{"a",integer()},{"b",integer()},{"v",vectors::type()},{"time",timestamps::type()}});
 for(std::int64_t i=0;i<3;++i)t.insert("t",{i,i+10,vectors::value(std::array{1.F,2.F}),timestamps::value(i)});
 t.create_table("empty",{{"a",integer()}});t.commit();
 auto tx=db.begin();
 expect(ErrorCode::type,[&]{tx.update("t",{{"b",call("fail_late",{column("a")})}});});
 CHECK(std::get<std::int64_t>(tx.query(Query{"t"}).rows[0][1])==10);
 expect(ErrorCode::type,[&]{tx.erase("t",Predicate{call("fail_late",{column("a")}),Compare::greater,literal(std::int64_t{0})});});
 CHECK(tx.query(Query{"t"}).rows.size()==3);
 expect(ErrorCode::schema,[&]{tx.update("empty",{{"missing",literal(std::int64_t{1})}});});
 expect(ErrorCode::type,[&]{tx.update("empty",{{"a",literal(1.0)}});});
 expect(ErrorCode::schema,[&]{tx.update("t",{{"a",column("b")},{"a",column("b")}});});
 expect(ErrorCode::schema,[&]{tx.update("t",{});});
 expect(ErrorCode::schema,[&]{tx.erase("missing");});
 CHECK(tx.update("t",{{"a",column("b")},{"b",column("a")}})==3);
 auto swapped=tx.query(Query{"t"});
 CHECK(std::get<std::int64_t>(swapped.rows[1][0])==11);
 CHECK(std::get<std::int64_t>(swapped.rows[1][1])==1);
 CHECK(std::get<std::int64_t>(db.query(Query{"t"}).rows[1][0])==1);
 CHECK(tx.update("t",{{"v",literal(vectors::value(std::array{3.F,4.F,5.F}))},{"time",literal(timestamps::value(-5))}},Predicate{column("b"),Compare::equal,literal(std::int64_t{1})})==1);
 CHECK(vectors::elements(tx.query(Query{"t"}).rows[1][2]).size()==3);
 CHECK(tx.erase("t",Predicate{column("time"),Compare::less,literal(timestamps::value(0))})==1);
 tx.rollback();CHECK(db.query(Query{"t"}).rows.size()==3);
 CHECK(vectors::elements(db.query(Query{"t"}).rows[1][2]).size()==2);
 auto a=db.begin(),b=db.begin();
 CHECK(a.erase("t",Predicate{column("a"),Compare::equal,literal(std::int64_t{1})})==1);a.commit();
 CHECK(b.query(Query{"t"}).rows.size()==3);
 expect(ErrorCode::conflict,[&]{b.commit();});b.rollback();
 CHECK(db.stats().rows==2);
 auto clear=db.begin();CHECK(clear.erase("t")==2);clear.commit();CHECK(db.stats().stored_payload_bytes==0);
 CHECK(db.stats().peak_stored_payload_bytes>0);
});}
