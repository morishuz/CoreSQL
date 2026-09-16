#include "check.hpp"
#include <sqlite3.h>
#include <random>
using namespace coresql;
struct SQLite {
    sqlite3* db=nullptr;
    SQLite(){CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);}
    ~SQLite(){sqlite3_close(db);}
    void exec(const std::string& s){if(sqlite3_exec(db,s.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
    std::vector<Row> query(const std::string& sql){sqlite3_stmt* stmt=nullptr;CHECK(sqlite3_prepare_v2(db,sql.c_str(),-1,&stmt,nullptr)==SQLITE_OK);
        struct Finalize{sqlite3_stmt* p;~Finalize(){sqlite3_finalize(p);}} cleanup{stmt};
        std::vector<Row> rows;int rc;while((rc=sqlite3_step(stmt))==SQLITE_ROW)rows.push_back({static_cast<std::int64_t>(sqlite3_column_int64(stmt,0)),static_cast<std::int64_t>(sqlite3_column_int64(stmt,1))});CHECK(rc==SQLITE_DONE);return rows;}
};
int main(){return tests([]{
    SQLite sql;Database db;auto tx=db.begin();
    for(auto table:{"l","keyed","plain","duplicates","texts","reals"}){
        const std::string name=table;
        tx.create_table(name,{{"id",integer(),name=="keyed"},{"group",integer()},{"label",text(),name=="texts"},{"number",real(),name=="reals"}});
        sql.exec("CREATE TABLE "+name+"(id INTEGER NOT NULL"+(name=="keyed"?" UNIQUE":"")+",g INTEGER NOT NULL,label TEXT NOT NULL"+(name=="texts"?" UNIQUE":"")+",number REAL NOT NULL"+(name=="reals"?" UNIQUE":"")+") STRICT");
        for(std::int64_t i=0;i<(name=="l"?200:100);++i){
            auto id=name=="duplicates"?i%11:i;auto group=name=="l"?i%130:i;auto label="key"+std::to_string(group);double number=double(group)/2;
            tx.insert(name,{id,group,label,number});sql.exec("INSERT INTO "+name+" VALUES("+std::to_string(id)+","+std::to_string(group)+",'"+label+"',"+std::to_string(number)+")");
        }
    }tx.commit();
    std::mt19937 random(731);
    for(auto table:{"keyed","plain","duplicates","texts","reals","l"})for(int trial=0;trial<60;++trial){
        std::string right=table;auto low=static_cast<std::int64_t>(random()%230),high=static_cast<std::int64_t>(random()%110);
        std::size_t limit=trial%3==0?random()%20:10000;bool desc=trial%2;
        auto lhs=right=="texts"?"label":right=="reals"?"number":"group";
        auto rhs=right=="texts"?"label":right=="reals"?"number":"id";
        Query q{"l",{column("a","id"),column("b","group")},all_of({
            {column("a","id"),Compare::greater_equal,literal(low)},
            {column("b","group"),Compare::less_equal,literal(high)}}),{Order{column("a","id"),desc}},limit};
        q.alias="a";q.join=InnerJoin{right,"b",column("a",lhs),column("b",rhs)};
        if(trial%2)std::swap(q.join->left,q.join->right);
        auto left_sql=std::string(lhs)=="group"?"g":lhs;
        auto expected=sql.query("SELECT a.id,b.g FROM l a INNER JOIN "+right+" b ON a."+left_sql+"=b."+rhs+" WHERE a.id>="+std::to_string(low)+" AND b.g<="+std::to_string(high)+" ORDER BY a.id "+(desc?"DESC":"ASC")+",b.g ASC LIMIT "+std::to_string(limit));
        auto actual=db.query(q).rows;CHECK(actual.size()==expected.size());
        for(std::size_t i=0;i<actual.size();++i)for(std::size_t j=0;j<2;++j)CHECK(std::get<std::int64_t>(actual[i][j])==std::get<std::int64_t>(expected[i][j]));
    }
});}
