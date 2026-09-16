#include "coresql/core.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <iostream>
using namespace coresql;
void check(bool b){if(!b)throw std::runtime_error("Join benchmark mismatch");}
struct SQLite{
    sqlite3* db=nullptr;SQLite(){check(sqlite3_open(":memory:",&db)==SQLITE_OK);}~SQLite(){sqlite3_close(db);}
    void exec(const std::string& s){if(sqlite3_exec(db,s.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
};
template<class F>double ms(F&& f){auto a=std::chrono::steady_clock::now();f();return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-a).count();}
int main(int argc,char**argv){try{
    const std::size_t n=argc==2?std::stoull(argv[1]):10000;check(n>=1000&&n<=100000);
    std::cerr<<"SQLite "<<sqlite3_libversion()<<" "<<sqlite3_sourceid()<<"\nBoth in memory; "<<n<<" documents, 1000 collections; seven alternating medians.\n"
        <<"Owned result rows and checks timed for both; SQLite prepared outside timing; CoreSQL binds each query. SQLite automatic indexes remain enabled.\n";
    Database core;SQLite sql;auto tx=core.begin();tx.create_table("documents",{{"id",integer()},{"collection",integer()}});
    sql.exec("CREATE TABLE documents(id INTEGER NOT NULL,collection INTEGER NOT NULL) STRICT;BEGIN");
    for(std::size_t i=0;i<n;++i){tx.insert("documents",{static_cast<std::int64_t>(i),static_cast<std::int64_t>(i%1000)});sql.exec("INSERT INTO documents VALUES("+std::to_string(i)+","+std::to_string(i%1000)+")");}
    for(auto table:{"keyed","plain"}){bool keyed=std::string(table)=="keyed";tx.create_table(table,{{"id",integer(),keyed},{"name",text()}});
        sql.exec("CREATE TABLE "+std::string(table)+"(id INTEGER NOT NULL"+(keyed?" UNIQUE":"")+",name TEXT NOT NULL) STRICT");
        for(std::int64_t i=0;i<1000;++i){tx.insert(table,{i,std::string("collection")+std::to_string(i)});sql.exec("INSERT INTO "+std::string(table)+" VALUES("+std::to_string(i)+",'collection"+std::to_string(i)+"')");}}
    tx.commit();sql.exec("COMMIT");
    auto verify=[&](const std::vector<Row>& rows){check(rows.size()==n);for(std::size_t i=0;i<n;++i){check(std::get<std::int64_t>(rows[i][0])==static_cast<std::int64_t>(i));check(std::get<std::string>(rows[i][1])=="collection"+std::to_string(i%1000));}};
    std::cout<<"workload,documents,collections,coresql_ms,sqlite_ms,coresql_over_sqlite\n";
    for(auto table:{"keyed","plain"}){
        Query q{"documents",{column("d","id"),column("c","name")},{},{Order{column("d","id")}}};q.alias="d";q.join=InnerJoin{table,"c",column("d","collection"),column("c","id")};
        sqlite3_stmt* stmt=nullptr;auto text="SELECT d.id,c.name FROM documents d JOIN "+std::string(table)+" c ON d.collection=c.id ORDER BY d.id";
        check(sqlite3_prepare_v2(sql.db,text.c_str(),-1,&stmt,nullptr)==SQLITE_OK);
        struct Finalize{sqlite3_stmt*p;~Finalize(){sqlite3_finalize(p);}} cleanup{stmt};
        auto a=[&]{verify(core.query(q).rows);};
        auto b=[&]{std::vector<Row> rows;int rc;while((rc=sqlite3_step(stmt))==SQLITE_ROW)rows.push_back({static_cast<std::int64_t>(sqlite3_column_int64(stmt,0)),std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt,1)),static_cast<std::size_t>(sqlite3_column_bytes(stmt,1)))});check(rc==SQLITE_DONE);check(sqlite3_reset(stmt)==SQLITE_OK);verify(rows);};
        a();b();std::vector<double> ca,sb;
        for(int i=0;i<7;++i){if(i%2){sb.push_back(ms(b));ca.push_back(ms(a));}else{ca.push_back(ms(a));sb.push_back(ms(b));}}
        std::sort(ca.begin(),ca.end());std::sort(sb.begin(),sb.end());std::cout<<table<<','<<n<<",1000,"<<ca[3]<<','<<sb[3]<<','<<ca[3]/sb[3]<<'\n';
    }
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
