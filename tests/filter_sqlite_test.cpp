#include "check.hpp"
#include <sqlite3.h>
#include <random>
#include <array>
using namespace coresql;
struct Condition {Predicate predicate;std::string sql;};
struct SQLite {
    sqlite3* db=nullptr;
    SQLite(){CHECK(sqlite3_open(":memory:",&db)==SQLITE_OK);}
    ~SQLite(){sqlite3_close(db);}
    void exec(const std::string& s){if(sqlite3_exec(db,s.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
    std::vector<Row> query(const std::string& s){
        sqlite3_stmt* stmt=nullptr;CHECK(sqlite3_prepare_v2(db,s.c_str(),-1,&stmt,nullptr)==SQLITE_OK);
        struct Finalize{sqlite3_stmt* p;~Finalize(){sqlite3_finalize(p);}} cleanup{stmt};
        std::vector<Row> rows;int rc;
        while((rc=sqlite3_step(stmt))==SQLITE_ROW)rows.push_back({static_cast<std::int64_t>(sqlite3_column_int64(stmt,0)),static_cast<std::int64_t>(sqlite3_column_int64(stmt,1))});
        CHECK(rc==SQLITE_DONE);return rows;
    }
};
int main(){return tests([]{
    std::cerr<<"Differential filters against SQLite "<<sqlite3_libversion()<<" "<<sqlite3_sourceid()<<'\n';
    SQLite sql;sql.exec("CREATE TABLE items(id INTEGER NOT NULL UNIQUE,value INTEGER NOT NULL,score REAL NOT NULL,name TEXT NOT NULL) STRICT;BEGIN");
    Database core;auto seed=core.begin();
    for(auto table:{"keyed","scan"})seed.create_table(table,{{"id",integer(),std::string(table)=="keyed"},{"value",integer()},{"score",real()},{"name",text()}});
    for(std::int64_t i=0;i<400;++i){
        Row row{i,i%17,double(i%11)/2,std::string(i%2?"alpha":"beta")};
        seed.insert("keyed",row);seed.insert("scan",row);
        sql.exec("INSERT INTO items VALUES("+std::to_string(i)+","+std::to_string(i%17)+","+std::to_string(double(i%11)/2)+",'"+(i%2?"alpha":"beta")+"')");
    }seed.commit();sql.exec("COMMIT");
    std::mt19937 random(8109);
    const std::array operations{Compare::equal,Compare::not_equal,Compare::less,Compare::greater,Compare::less_equal,Compare::greater_equal};
    const std::array<const char*,6> symbols{"=","!=","<",">","<=",">="};
    std::function<Condition(unsigned)> generate=[&](unsigned depth)->Condition {
        if(depth==0||random()%3==0){
            const auto op=random()%6, field=random()%4;
            std::string name;Value value;std::string literal_sql;
            if(field==0||field==1){name=field==0?"id":"value";auto n=static_cast<std::int64_t>(random()%(field==0?450:20))-2;value=n;literal_sql=std::to_string(n);}
            else if(field==2){name="score";auto n=double(random()%13)/2;value=n;literal_sql=std::to_string(n);}
            else{name="name";std::string n=random()%2?"alpha":"beta";value=n;literal_sql="'"+n+"'";}
            if(random()%2)return {{column(name),operations[op],literal(value)},"("+name+symbols[op]+literal_sql+")"};
            return {{literal(value),operations[op],column(name)},"("+literal_sql+symbols[op]+name+")"};
        }
        auto kind=random()%3;
        if(kind==0){auto child=generate(depth-1);return {not_(std::move(child.predicate)),"(NOT "+child.sql+")"};}
        std::vector<Predicate> children;std::string text;
        const auto count=random()%4;
        for(unsigned i=0;i<count;++i){auto child=generate(depth-1);if(i)text+=kind==1?" AND ":" OR ";text+=child.sql;children.push_back(std::move(child.predicate));}
        if(!count)text=kind==1?"1":"0";
        return {kind==1?all_of(std::move(children)):any_of(std::move(children)),"("+text+")"};
    };
    auto check_rows=[](const std::vector<Row>& a,const std::vector<Row>& b){CHECK(a.size()==b.size());
        for(std::size_t i=0;i<a.size();++i)for(std::size_t j=0;j<2;++j)CHECK(std::get<std::int64_t>(a[i][j])==std::get<std::int64_t>(b[i][j]));};
    for(int trial=0;trial<400;++trial){
        auto condition=generate(4);
        if(trial%4==0){auto id=static_cast<std::int64_t>(random()%450);condition={all_of({{column("id"),Compare::equal,literal(id)},condition.predicate}),"(id="+std::to_string(id)+" AND "+condition.sql+")"};}
        std::size_t limit=random()%3?500:random()%30;
        auto expected=sql.query("SELECT id,value FROM items WHERE "+condition.sql+" ORDER BY id LIMIT "+std::to_string(limit));
        for(auto table:{"keyed","scan"}){
            check_rows(core.query(Query{table,{column("id"),column("value")},condition.predicate,{Order{column("id")}},limit}).rows,expected);
            auto tx=core.begin();sql.exec("BEGIN");std::size_t count;
            if(trial%2){count=tx.erase(table,condition.predicate);sql.exec("DELETE FROM items WHERE "+condition.sql);}
            else{count=tx.update(table,{{"value",literal(std::int64_t{-7})}},condition.predicate);sql.exec("UPDATE items SET value=-7 WHERE "+condition.sql);}
            CHECK(count==static_cast<std::size_t>(sqlite3_changes(sql.db)));
            check_rows(tx.query(Query{table,{column("id"),column("value")},{},{Order{column("id")}}}).rows,sql.query("SELECT id,value FROM items ORDER BY id"));
            tx.rollback();sql.exec("ROLLBACK");
        }
    }
});}
