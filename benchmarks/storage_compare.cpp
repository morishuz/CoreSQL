#include "coresql/core.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <sys/resource.h>
#include <unistd.h>
using namespace coresql;
namespace {
void check(bool ok) { if(!ok) throw std::runtime_error("Storage benchmark check failed"); }
// Count bytes requested at SQLite's VFS boundary, including rollback journals.
// Version 1 deliberately exposes rollback-journal I/O only; no WAL/shm methods.
std::uint64_t sql_bytes=0;
sqlite3_vfs* native=nullptr;
struct alignas(std::max_align_t) File { sqlite3_file base; sqlite3_file* inner; };
File* file(sqlite3_file* f){return reinterpret_cast<File*>(f);}
int close_file(sqlite3_file* f){return file(f)->inner->pMethods->xClose(file(f)->inner);}
int read_file(sqlite3_file* f,void* p,int n,sqlite3_int64 o){return file(f)->inner->pMethods->xRead(file(f)->inner,p,n,o);}
int write_file(sqlite3_file* f,const void* p,int n,sqlite3_int64 o){sql_bytes+=static_cast<std::uint64_t>(n);return file(f)->inner->pMethods->xWrite(file(f)->inner,p,n,o);}
int truncate_file(sqlite3_file* f,sqlite3_int64 n){return file(f)->inner->pMethods->xTruncate(file(f)->inner,n);}
int sync_file(sqlite3_file* f,int n){return file(f)->inner->pMethods->xSync(file(f)->inner,n);}
int size_file(sqlite3_file* f,sqlite3_int64* n){return file(f)->inner->pMethods->xFileSize(file(f)->inner,n);}
int lock_file(sqlite3_file* f,int n){return file(f)->inner->pMethods->xLock(file(f)->inner,n);}
int unlock_file(sqlite3_file* f,int n){return file(f)->inner->pMethods->xUnlock(file(f)->inner,n);}
int reserved_file(sqlite3_file* f,int* n){return file(f)->inner->pMethods->xCheckReservedLock(file(f)->inner,n);}
int control_file(sqlite3_file* f,int n,void* p){return file(f)->inner->pMethods->xFileControl(file(f)->inner,n,p);}
int sector_file(sqlite3_file* f){return file(f)->inner->pMethods->xSectorSize(file(f)->inner);}
int device_file(sqlite3_file* f){return file(f)->inner->pMethods->xDeviceCharacteristics(file(f)->inner);}
const sqlite3_io_methods methods={1,close_file,read_file,write_file,truncate_file,sync_file,size_file,lock_file,unlock_file,reserved_file,control_file,sector_file,device_file,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
int open_file(sqlite3_vfs*,const char* name,sqlite3_file* out,int flags,int* actual) {
    auto* wrapped=file(out);wrapped->inner=reinterpret_cast<sqlite3_file*>(wrapped+1);
    int rc=native->xOpen(native,name,wrapped->inner,flags,actual);
    wrapped->base.pMethods=rc==SQLITE_OK?&methods:nullptr;return rc;
}
void install_vfs(){native=sqlite3_vfs_find(nullptr);check(native);static sqlite3_vfs v=*native;v.zName="coresql-count";v.szOsFile=static_cast<int>(sizeof(File))+native->szOsFile;v.xOpen=open_file;check(sqlite3_vfs_register(&v,0)==SQLITE_OK);}
struct Engine {
    virtual ~Engine()=default;
    virtual void open()=0;
    virtual void close()=0;
    virtual void seed(int)=0;
    virtual void update(int,int)=0;
    virtual void insert(int)=0;
    virtual void erase(int)=0;
    virtual std::size_t count()=0;
    virtual std::uint64_t written()=0;
    virtual void compact()=0;
    virtual void visit(const std::function<void(std::int64_t,std::int64_t,const std::string&)>&)=0;
};
struct Core final:Engine {
    std::filesystem::path path;std::optional<Database> db;std::uint64_t prior=0;
    explicit Core(std::filesystem::path p):path(std::move(p)){}
    void open()override{db.emplace(Database::open(path));}
    void close()override{prior+=db->storage_stats().bytes_written;db.reset();}
    std::uint64_t written()override{return prior+(db?db->storage_stats().bytes_written:0);}
    void seed(int n)override{auto t=db->begin();t.create_table("t",{{"id",integer()},{"value",integer()},{"text",text()}});for(int i=0;i<n;++i)t.insert("t",{std::int64_t{i},std::int64_t{0},std::string(64,'x')});t.commit();}
    void update(int id,int v)override{auto t=db->begin();check(t.update("t",{{"value",literal(std::int64_t{v})}},Predicate{column("id"),Compare::equal,literal(std::int64_t{id})})==1);t.commit();}
    void insert(int first)override{auto t=db->begin();for(int i=first;i<first+100;++i)t.insert("t",{std::int64_t{i},std::int64_t{0},std::string(64,'x')});t.commit();}
    void erase(int bound)override{auto t=db->begin();check(t.erase("t",Predicate{column("id"),Compare::less,literal(std::int64_t{bound})})==100);t.commit();}
    std::size_t count()override{return db->stats().rows;}
    void compact()override{db->checkpoint();}
    void visit(const std::function<void(std::int64_t,std::int64_t,const std::string&)>& f)override{
        for(const auto& row:db->query(Query{"t"}).rows)f(std::get<std::int64_t>(row[0]),std::get<std::int64_t>(row[1]),std::get<std::string>(row[2]));
    }
};
struct Sql final:Engine {
    std::filesystem::path path;sqlite3* db=nullptr;
    explicit Sql(std::filesystem::path p):path(std::move(p)){}
    ~Sql(){if(db)sqlite3_close(db);}
    void exec(const std::string& s){if(sqlite3_exec(db,s.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
    void open()override{
        check(sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,"coresql-count")==SQLITE_OK);
        exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA; PRAGMA fullfsync=ON; PRAGMA mmap_size=0;");
    }
    void close()override{check(sqlite3_close(db)==SQLITE_OK);db=nullptr;}
    std::uint64_t written()override{return sql_bytes;}
    void seed(int n)override{exec("CREATE TABLE t(id INTEGER,value INTEGER,text TEXT) STRICT; BEGIN");add(0,n);exec("COMMIT");}
    void add(int first,int n){
        sqlite3_stmt* s=nullptr;check(sqlite3_prepare_v2(db,"INSERT INTO t VALUES(?,0,?)",-1,&s,nullptr)==SQLITE_OK);
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> guard(s,sqlite3_finalize);
        std::string text(64,'x');check(sqlite3_bind_text(s,2,text.c_str(),64,SQLITE_TRANSIENT)==SQLITE_OK);
        for(int i=first;i<first+n;++i){check(sqlite3_bind_int(s,1,i)==SQLITE_OK);check(sqlite3_step(s)==SQLITE_DONE);check(sqlite3_reset(s)==SQLITE_OK);}
    }
    void update(int id,int v)override{exec("BEGIN; UPDATE t SET value="+std::to_string(v)+" WHERE id="+std::to_string(id));check(sqlite3_changes(db)==1);exec("COMMIT");}
    void insert(int first)override{exec("BEGIN");add(first,100);exec("COMMIT");}
    void erase(int bound)override{exec("BEGIN; DELETE FROM t WHERE id<"+std::to_string(bound));check(sqlite3_changes(db)==100);exec("COMMIT");}
    std::size_t count()override{sqlite3_stmt* s=nullptr;check(sqlite3_prepare_v2(db,"SELECT count(*) FROM t",-1,&s,nullptr)==SQLITE_OK);std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> guard(s,sqlite3_finalize);check(sqlite3_step(s)==SQLITE_ROW);return static_cast<std::size_t>(sqlite3_column_int64(s,0));}
    void compact()override{exec("VACUUM");}
    void visit(const std::function<void(std::int64_t,std::int64_t,const std::string&)>& f)override{
        sqlite3_stmt* s=nullptr;check(sqlite3_prepare_v2(db,"SELECT id,value,text FROM t",-1,&s,nullptr)==SQLITE_OK);
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> guard(s,sqlite3_finalize);int rc;
        while((rc=sqlite3_step(s))==SQLITE_ROW)f(sqlite3_column_int64(s,0),sqlite3_column_int64(s,1),std::string(reinterpret_cast<const char*>(sqlite3_column_text(s,2)),static_cast<std::size_t>(sqlite3_column_bytes(s,2))));
        check(rc==SQLITE_DONE);
    }
};
template<class F>void measure(const char* engine,const char* operation,int n,int repeats,Engine& db,F&& f){
    std::vector<double> times;auto before=db.written();
    for(int i=0;i<repeats;++i){auto start=std::chrono::steady_clock::now();f(i);times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
    std::sort(times.begin(),times.end());
    rusage usage{};check(getrusage(RUSAGE_SELF,&usage)==0);
#ifdef __APPLE__
    auto peak=usage.ru_maxrss;
#else
    auto peak=usage.ru_maxrss*1024;
#endif
    std::cout<<engine<<','<<operation<<','<<n<<','<<repeats<<','<<times[times.size()/2]<<','<<times[static_cast<std::size_t>(std::ceil(times.size()*0.95))-1]<<','<<(db.written()-before)<<','<<peak<<'\n';
}
}
int main(int argc,char** argv){
    std::filesystem::path directory;
    try {
        check(argc==3);std::string engine=argv[1];int n=std::stoi(argv[2]);check(n>=2000&&n<=200000);
        auto pattern=(std::filesystem::temp_directory_path()/"coresql-storage-bench-XXXXXX").string();auto* p=mkdtemp(pattern.data());check(p);directory=p;
        install_vfs();std::unique_ptr<Engine> db;
        if(engine=="coresql")db=std::make_unique<Core>(directory/"db");else {check(engine=="sqlite");db=std::make_unique<Sql>(directory/"db");}
        std::cerr<<"SQLite "<<sqlite3_sourceid()<<"; compiler="<<__VERSION__<<"; local disk, no indexes; SQLite DELETE/EXTRA/fullfsync; process peak RSS includes setup\n";
        std::cout<<"engine,operation,rows,repeats,median_ms,p95_ms,bytes_written_total,process_peak_rss_bytes\n";
        db->open();db->seed(n);check(db->count()==static_cast<std::size_t>(n));
        measure(engine.c_str(),"update_one_commit",n,20,*db,[&](int i){db->update(n/2+i,i+1);});
        measure(engine.c_str(),"insert_100_commit",n,10,*db,[&](int i){db->insert(n+i*100);});
        measure(engine.c_str(),"delete_100_commit",n,10,*db,[&](int i){db->erase((i+1)*100);});
        check(db->count()==static_cast<std::size_t>(n));
        db->close();
        measure(engine.c_str(),"reopen_and_count",n,10,*db,[&](int){db->open();check(db->count()==static_cast<std::size_t>(n));db->close();});
        db->open();measure(engine.c_str(),"checkpoint_or_vacuum",n,1,*db,[&](int){db->compact();});
        check(db->count()==static_cast<std::size_t>(n));
        std::vector<bool> seen(static_cast<std::size_t>(n+1000));std::size_t checked=0;
        db->visit([&](std::int64_t id,std::int64_t value,const std::string& text){
            check(id>=1000&&id<n+1000);auto index=static_cast<std::size_t>(id);check(!seen[index]);seen[index]=true;
            check(value==((id>=n/2&&id<n/2+20)?id-n/2+1:0));check(text==std::string(64,'x'));++checked;
        });
        check(checked==static_cast<std::size_t>(n));db->close();db.reset();std::filesystem::remove_all(directory);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(!directory.empty()){std::error_code ec;std::filesystem::remove_all(directory,ec);}return 1;}
}
