#include "check.hpp"
#include "coresql/sql.hpp"
#include "coresql/timestamp.hpp"
#include "coresql/vector.hpp"
#include <array>
using namespace coresql;
int main(){return tests([]{
    Registry registry;timestamps::install(registry);vectors::install(registry);
    registry.add(Function{"fail",[](std::span<const Type> t){CHECK(t.empty());return integer();},[](std::span<const Value>)->Value{throw Error(ErrorCode::constraint,"Callback failure");}});
    sql::install(registry);Database db(registry);auto tx=db.begin();
    tx.create_table("l",{{"id",integer()},{"stamp",timestamps::type()}});
    tx.create_table("r",{{"id",integer()},{"stamp",timestamps::type()}});
    tx.insert("l",{std::int64_t{1},timestamps::value(10)});tx.insert("l",{std::int64_t{2},timestamps::value(20)});
    tx.insert("r",{std::int64_t{3},timestamps::value(20)});tx.insert("r",{std::int64_t{4},timestamps::value(30)});tx.commit();
    Query q{"l",{column("a","stamp"),column("b","stamp")}};q.alias="a";
    q.join=Join{"r","b",column("a","stamp"),column("b","stamp")};q.join->kind=JoinKind::full;
    auto rows=db.query(q);CHECK(rows.types==std::vector<Type>({timestamps::type(),timestamps::type()}));CHECK(rows.rows.size()==3);
    CHECK(rows.rows[0][1]==Value(Null(timestamps::type())));CHECK(rows.rows[2][0]==Value(Null(timestamps::type())));
    q.select={aggregate("count",{column("b","stamp")},true)};CHECK(db.query(q).rows[0][0]==Value(std::int64_t{2}));
    q.join->on=Predicate{call("fail",{}),Compare::equal,literal(std::int64_t{1})};q.limit=0;CHECK(db.query(q).rows.empty());
    q.limit=1;expect(ErrorCode::constraint,[&]{db.query(q);});
    q.select={column("a","missing")};expect(ErrorCode::schema,[&]{db.query(q);});
    sql::Connection sql(db,registry);
    CHECK(sql.execute("SELECT count(DISTINCT stamp) FROM l").rows[0][0]==Value(std::int64_t{2}));
    CHECK(sql.execute("SELECT CASE WHEN 1 THEN stamp ELSE NULL END FROM l ORDER BY id").rows[0][0]==timestamps::value(10));
    expect(ErrorCode::type,[&]{sql.execute("SELECT CASE WHEN 1 THEN stamp ELSE 1 END FROM l LIMIT 0");});
    expect(ErrorCode::type,[&]{sql.execute("SELECT CAST(stamp AS INTEGER) FROM l LIMIT 0");});
    tx=db.begin();tx.create_table("v",{{"x",vectors::type()}});tx.insert("v",{vectors::value(std::array{1.0F,2.0F})});tx.commit();
    expect(ErrorCode::unsupported,[&]{sql.execute("SELECT count(DISTINCT x) FROM v LIMIT 0");});
});}
