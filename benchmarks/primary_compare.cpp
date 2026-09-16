#include "coresql/core.hpp"
#include "coresql/timestamp.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <unistd.h>
using namespace coresql;
using Clock=std::chrono::steady_clock;
void check(bool b){if(!b)throw std::runtime_error("Benchmark correctness check failed");}
struct SQLite {
    sqlite3* db=nullptr;
    SQLite(){check(sqlite3_open(":memory:",&db)==SQLITE_OK);}
    ~SQLite(){sqlite3_close(db);}
    void exec(const char* s){if(sqlite3_exec(db,s,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
};
struct Statement {
    sqlite3_stmt* p=nullptr;
    Statement(SQLite& db,const char* s){check(sqlite3_prepare_v2(db.db,s,-1,&p,nullptr)==SQLITE_OK);}
    ~Statement(){sqlite3_finalize(p);}
    void bind(std::int64_t n){check(sqlite3_bind_int64(p,1,n)==SQLITE_OK);}
    int step(){auto rc=sqlite3_step(p);check(rc==SQLITE_ROW||rc==SQLITE_DONE);return rc;}
    void reset(){check(sqlite3_reset(p)==SQLITE_OK);}
};
template<class F> double timing(F&& f){auto start=Clock::now();f();return std::chrono::duration<double,std::micro>(Clock::now()-start).count();}
template<class A,class B> void compare(const std::string& name,std::size_t n,int operations,A&& a,B&& b){
    a();b();std::vector<double> ca,sb;
    for(int i=0;i<7;++i){if(i%2){sb.push_back(timing(b));ca.push_back(timing(a));}else{ca.push_back(timing(a));sb.push_back(timing(b));}}
    std::sort(ca.begin(),ca.end());std::sort(sb.begin(),sb.end());
    std::cout<<name<<','<<n<<','<<ca[3]/operations<<','<<sb[3]/operations<<','<<ca[3]/sb[3]<<'\n';
}
Predicate by(std::int64_t id){return {column("id"),Compare::equal,literal(id)};}
void seed(Database& db,std::size_t n,bool keyed){auto t=db.begin();t.create_table("items",{{"id",integer(),keyed},{"value",integer()}});
    for(std::size_t i=0;i<n;++i)t.insert("items",{static_cast<std::int64_t>(i),static_cast<std::int64_t>(i)});t.commit();}
int main(int argc,char** argv){try{
    std::size_t n=argc==2?std::stoull(argv[1]):100000;check(n>=100&&n<=1000000);
    std::cerr<<"SQLite "<<sqlite3_libversion()<<" source="<<sqlite3_sourceid()<<"\ncompiler="<<__VERSION__
        <<"\nBoth in memory; SQLite statements prepared outside timing. Seven alternating trials, median microseconds per operation.\n"
        <<"SQLite indexed schema uses INTEGER NOT NULL UNIQUE (explicit index, not rowid primary key). CoreSQL binds each query.\n";
    std::cout<<"workload,rows,coresql_us,sqlite_us,coresql_over_sqlite\n";
    for(bool keyed:{false,true}){
        Database core;seed(core,n,keyed);SQLite sql;
        sql.exec(keyed?"CREATE TABLE items(id INTEGER NOT NULL UNIQUE,value INTEGER NOT NULL) STRICT":"CREATE TABLE items(id INTEGER NOT NULL,value INTEGER NOT NULL) STRICT");
        Statement insert(sql,"INSERT INTO items VALUES(?1,?1)");sql.exec("BEGIN");
        for(std::size_t i=0;i<n;++i){insert.bind(static_cast<std::int64_t>(i));check(insert.step()==SQLITE_DONE);insert.reset();}sql.exec("COMMIT");
        const std::string prefix=keyed?"keyed_":"scan_";
        Statement select(sql,"SELECT value FROM items WHERE id=?1");
        auto id=[&](int i){return static_cast<std::int64_t>((static_cast<std::size_t>(i)*7919)%n);};
        compare(prefix+"lookup",n,100,[&]{for(int i=0;i<100;++i){auto rows=core.query(Query{"items",{column("value")},by(id(i))}).rows;
                check(rows.size()==1&&std::get<std::int64_t>(rows[0][0])==id(i));}},
            [&]{for(int i=0;i<100;++i){select.bind(id(i));std::vector<Row> rows;while(select.step()==SQLITE_ROW)rows.push_back({static_cast<std::int64_t>(sqlite3_column_int64(select.p,0))});select.reset();
                check(rows.size()==1&&std::get<std::int64_t>(rows[0][0])==id(i));}});
        compare(prefix+"missing",n,100,[&]{for(int i=0;i<100;++i)check(core.query(Query{"items",{column("value")},by(-1)}).rows.empty());},
            [&]{for(int i=0;i<100;++i){select.bind(-1);check(select.step()==SQLITE_DONE);select.reset();}});
        for(int operation=0;operation<3;++operation){
            const char* sqltext=operation==0?"UPDATE items SET value=-1 WHERE id=?1":operation==1?"DELETE FROM items WHERE id=?1":"UPDATE items SET id=-1 WHERE id=?1";
            const char* label=operation==0?"update_rollback":operation==1?"delete_rollback":"rekey_rollback";
            Statement statement(sql,sqltext);
            compare(prefix+label,n,50,[&]{for(int i=0;i<50;++i){auto t=core.begin();auto changed=operation==1?t.erase("items",by(id(i))):t.update("items",{{operation==0?"value":"id",literal(std::int64_t{-1})}},by(id(i)));check(changed==1);t.rollback();}},
                [&]{for(int i=0;i<50;++i){sql.exec("BEGIN");statement.bind(id(i));check(statement.step()==SQLITE_DONE);check(sqlite3_changes(sql.db)==1);statement.reset();sql.exec("ROLLBACK");}});
        }
        if (keyed) {
            Statement missing_update(sql, "UPDATE items SET value=0 WHERE id=-1");
            compare("keyed_missing_update_rollback",n,50,[&]{for(int i=0;i<50;++i){auto t=core.begin();
                check(t.update("items",{{"value",literal(std::int64_t{0})}},by(-1))==0);t.rollback();}},
                [&]{for(int i=0;i<50;++i){sql.exec("BEGIN");check(missing_update.step()==SQLITE_DONE);
                    check(sqlite3_changes(sql.db)==0);missing_update.reset();sql.exec("ROLLBACK");}});
            compare("keyed_duplicate_insert_rollback",n,50,[&]{for(int i=0;i<50;++i){auto t=core.begin();
                bool rejected=false;try{t.insert("items",{id(i),id(i)});}catch(const Error& e){check(e.code==ErrorCode::constraint);rejected=true;}
                check(rejected);t.rollback();}},
                [&]{for(int i=0;i<50;++i){sql.exec("BEGIN");insert.bind(id(i));check(sqlite3_step(insert.p)==SQLITE_CONSTRAINT);
                    check(sqlite3_reset(insert.p)==SQLITE_CONSTRAINT);sql.exec("ROLLBACK");}});
        }
        compare(prefix+"delete_all_rollback",n,3,[&]{for(int i=0;i<3;++i){auto t=core.begin();check(t.erase("items")==n);t.rollback();}},
            [&]{for(int i=0;i<3;++i){sql.exec("BEGIN");sql.exec("DELETE FROM items");check(static_cast<std::size_t>(sqlite3_changes(sql.db))==n);sql.exec("ROLLBACK");}});
        compare(prefix+"insert_rollback",n,50,[&]{for(int i=0;i<50;++i){auto t=core.begin();t.insert("items",{std::int64_t{-1},std::int64_t{-1}});t.rollback();}},
            [&]{for(int i=0;i<50;++i){sql.exec("BEGIN");insert.bind(-1);check(insert.step()==SQLITE_DONE);insert.reset();sql.exec("ROLLBACK");}});
    }
    // CoreSQL-only comparison: identical committed delete/reinsert churn with
    // and without retaining all 20 historical snapshots until the batch ends.
    {
        Database plain, retained;seed(plain,n,true);seed(retained,n,true);
        std::int64_t generation=0;
        auto churn=[&](Database& db,bool keep){
            std::vector<Transaction> snapshots;
            auto old=std::get<std::int64_t>(db.query(Query{"items",{column("value")},by(0)}).rows[0][0]);
            for(int i=0;i<20;++i){if(keep)snapshots.push_back(db.begin());auto tx=db.begin();
                check(tx.erase("items",by(i))==1);tx.insert("items",{std::int64_t{i},++generation});tx.commit();}
            check(db.stats().rows==n);
            if(keep)check(std::get<std::int64_t>(snapshots.front().query(Query{"items",{column("value")},by(0)}).rows[0][0])==old);
        };
        std::cerr<<"Next churn row compares CoreSQL without/with 20 retained snapshots, not SQLite.\n";
        compare("core_churn_plain_vs_retained",n,20,[&]{churn(plain,false);},[&]{churn(retained,true);});
    }
    // Extension sorting versus the equivalent SQLite signed integer ordering.
    {
        Registry registry; timestamps::install(registry); Database core(registry); SQLite sql;
        auto tx=core.begin();tx.create_table("times",{{"id",integer()},{"stamp",timestamps::type()}});
        sql.exec("CREATE TABLE times(id INTEGER NOT NULL,stamp INTEGER NOT NULL) STRICT");
        Statement insert(sql,"INSERT INTO times VALUES(?1,?2)");sql.exec("BEGIN");
        for(std::size_t i=0;i<n;++i){auto id=static_cast<std::int64_t>(i);auto stamp=static_cast<std::int64_t>((i*7919)%n);
            tx.insert("times",{id,timestamps::value(stamp)});insert.bind(id);
            check(sqlite3_bind_int64(insert.p,2,stamp)==SQLITE_OK);check(insert.step()==SQLITE_DONE);insert.reset();}
        tx.commit();sql.exec("COMMIT");
        for(bool limited:{false,true}) {
            Query query{"times",{column("id")},{},{Order{column("stamp")}},limited?10:n};
            Statement select(sql,limited?"SELECT id FROM times ORDER BY stamp,id LIMIT 10":"SELECT id FROM times ORDER BY stamp,id");
            std::vector<std::int64_t> expected;while(select.step()==SQLITE_ROW)expected.push_back(sqlite3_column_int64(select.p,0));select.reset();
            compare(limited?"timestamp_top10":"timestamp_sort",n,3,[&]{for(int i=0;i<3;++i){auto result=core.query(query);
                check(result.rows.size()==expected.size());for(std::size_t j=0;j<expected.size();++j)check(std::get<std::int64_t>(result.rows[j][0])==expected[j]);}},
                [&]{for(int i=0;i<3;++i){std::vector<Row> rows;while(select.step()==SQLITE_ROW)rows.push_back({static_cast<std::int64_t>(sqlite3_column_int64(select.p,0))});select.reset();
                    check(rows.size()==expected.size());for(std::size_t j=0;j<expected.size();++j)check(std::get<std::int64_t>(rows[j][0])==expected[j]);}});
        }
    }
    // Recovery includes reading/decoding all rows and building the optional
    // index. OS cache is warm; this is not an SQLite open-latency comparison.
    auto directory=std::filesystem::temp_directory_path()/("coresql-key-bench-"+std::to_string(::getpid()));
    check(std::filesystem::create_directory(directory));
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{directory};
    for(bool keyed:{false,true}){auto path=directory/(keyed?"keyed":"plain");auto db=Database::open(path);seed(db,n,keyed);db.checkpoint();}
    auto reopen=[&](const char* name){auto db=Database::open(directory/name);check(db.stats().rows==n);};
    std::cerr<<"Next row uses columns CoreSQL keyed reopen us, CoreSQL unkeyed reopen us, ratio; warm OS cache; row count verified.\n";
    compare("core_reopen_keyed_vs_unkeyed",n,1,[&]{reopen("keyed");},[&]{reopen("plain");});
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
