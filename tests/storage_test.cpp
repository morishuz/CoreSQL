#include "check.hpp"
#include "coresql/vector.hpp"
#include "coresql/timestamp.hpp"
#include "../src/storage.hpp"
#include <array>
#include <cstring>
#include <sys/wait.h>
using namespace coresql;
namespace {
const char* stop_stage = nullptr;
bool crash = true;
int notify_fd = -1, resume_fd = -1;
void race(const char* stage) {
    if (std::strcmp(stage,"open_before_lock") != 0) return;
    char byte = 'x';
    if (::write(notify_fd,&byte,1) != 1 || ::read(resume_fd,&byte,1) != 1) ::_exit(80);
}
void stop(const char* stage) {
    if(std::strcmp(stage,stop_stage)==0) {
        if(crash) ::_exit(77);
        throw Error(ErrorCode::io,"Injected write/sync failure");
    }
}
Registry registry(){Registry r;vectors::install(r);timestamps::install(r);return r;}
void seed(const std::filesystem::path& path) {
    auto db=Database::open(path,registry());CHECK(db.persistent());
    auto tx=db.begin();
    tx.create_table("t",{{"id",integer()},{"v",vectors::type()},{"time",timestamps::type()}});
    tx.insert("t",{std::int64_t{1},vectors::value(std::array{1.F,2.F}),timestamps::value(-10)});tx.commit();
}
void change(Database& db) {
    auto tx=db.begin();
    tx.update("t",{{"id",literal(std::int64_t{2})},{"v",literal(vectors::value(std::array{3.F,4.F,5.F}))},{"time",literal(timestamps::value(20))}});
    tx.commit();
}
std::int64_t id(Database& db){return std::get<std::int64_t>(db.query(Query{"t"}).rows[0][0]);}
}
int main(){return tests([]{
 TempDirectory temp;
 auto path=temp.path/"live";seed(path);
 {
    auto db=Database::open(path,registry());CHECK(id(db)==1);
    expect(ErrorCode::conflict,[&]{Database::open(path,registry());});
    auto size=std::filesystem::file_size(path);
    {auto tx=db.begin();tx.erase("t");tx.rollback();}
    {auto tx=db.begin();tx.commit();}
    CHECK(std::filesystem::file_size(path)==size);
    auto stale=db.begin();change(db);
    expect(ErrorCode::conflict,[&]{stale.commit();});
    CHECK(id(db)==2);
    db.save(temp.path/"export");
 }
 {auto db=Database::open(path,registry());CHECK(id(db)==2);
  auto row=db.query(Query{"t"}).rows[0];
  CHECK(vectors::elements(row[1])==std::vector<float>({3,4,5}));CHECK(timestamps::microseconds(row[2])==20);
  auto tx=db.begin();tx.erase("t");tx.commit();}
 {auto db=Database::open(path,registry());CHECK(db.stats().rows==0);}
 CHECK(!Database::load(temp.path/"export",registry()).persistent());
 expect(ErrorCode::type,[&]{Database::open(path);}); // Schema still references add-ons.
 // Forked writer terminates at real I/O boundaries, without destructors/rollback.
 const char* stages[]={"before_write","half_header","header","half_payload","payload","payload_synced","half_footer","footer","before_sync","synced"};
 for(auto stage:stages) {
    auto file=temp.path/stage;seed(file);
    pid_t pid=::fork();CHECK(pid>=0);
    if(pid==0) {
        try {auto db=Database::open(file,registry());stop_stage=stage;crash=true;detail::storage_hook(stop);change(db);::_exit(78);}
        catch(...){::_exit(79);}
    }
    int status=0;CHECK(::waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==77);
    auto db=Database::open(file,registry());
    bool published=std::strcmp(stage,"footer")==0||std::strcmp(stage,"before_sync")==0||std::strcmp(stage,"synced")==0;
    CHECK(id(db)==(published?2:1));
    change(db);CHECK(id(db)==2); // Recovery correctly positions the next append.
 }
 // Errors make the live handle unusable: the caller must resolve the outcome by reopening.
 for(auto stage:{"half_header","half_payload","before_sync","synced"}) {
    auto file=temp.path/(std::string("failure-")+stage);seed(file);
    std::optional<Database> db;db.emplace(Database::open(file,registry()));
    stop_stage=stage;crash=false;detail::storage_hook(stop);
    expect(ErrorCode::io,[&]{change(*db);});detail::storage_hook(nullptr);
    expect(ErrorCode::state,[&]{db->begin();});expect(ErrorCode::state,[&]{db->query(Query{"t"});});
    db.reset();db.emplace(Database::open(file,registry()));
    CHECK(id(*db)==((std::strcmp(stage,"before_sync")==0||std::strcmp(stage,"synced")==0)?2:1));
 }
 // Every truncated byte position in a new record recovers the previous commit.
 auto complete=temp.path/"complete";seed(complete);
 auto old_size=std::filesystem::file_size(complete);
 {auto db=Database::open(complete,registry());change(db);}
 auto full_size=std::filesystem::file_size(complete);
 for(auto length=old_size;length<full_size;++length) {
    auto file=temp.path/"truncated";
    std::filesystem::copy_file(complete,file,std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(file,length);
    {auto db=Database::open(file,registry());CHECK(id(db)==1);CHECK(std::filesystem::file_size(file)==old_size);}
 }
 auto damaged=temp.path/"damaged";
 std::filesystem::copy_file(complete,damaged);
 {std::fstream f(damaged,std::ios::in|std::ios::out|std::ios::binary);f.seekg(static_cast<std::streamoff>(old_size+30));char c;f.get(c);c^=1;f.seekp(static_cast<std::streamoff>(old_size+30));f.put(c);}
 expect(ErrorCode::format,[&]{Database::open(damaged,registry());});
 // A shrink forces automatic checkpointing. Interrupt every publication phase.
 const char* checkpoint_stages[]={"checkpoint_created","half_header","half_payload","payload_synced","half_footer","synced","checkpoint_synced","checkpoint_renamed","checkpoint_directory_synced"};
 auto large=temp.path/"large";
 {auto db=Database::open(large);auto tx=db.begin();tx.create_table("big",{{"id",integer()},{"s",text()}});
  for(std::int64_t i=0;i<5000;++i)tx.insert("big",{i,std::string(300,'x')});tx.commit();}
 for(auto stage:checkpoint_stages) {
    auto file=temp.path/(std::string("compact-")+stage);std::filesystem::copy_file(large,file);
    pid_t pid=::fork();CHECK(pid>=0);
    if(pid==0) {
      try {auto db=Database::open(file);stop_stage=stage;crash=true;detail::storage_hook(stop);
           auto tx=db.begin();tx.erase("big");tx.commit();::_exit(78);}catch(...){::_exit(79);}
    }
    int status=0;CHECK(::waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==77);
    auto db=Database::open(file);
    bool published=std::strcmp(stage,"checkpoint_renamed")==0||std::strcmp(stage,"checkpoint_directory_synced")==0;
    CHECK(db.stats().rows==(published?0:5000));
    CHECK(!std::filesystem::exists(file.string()+".checkpoint"));
    expect(ErrorCode::conflict,[&]{Database::open(file);});
    auto tx=db.begin();tx.erase("big");tx.insert("big",{std::int64_t{9},std::string("ok")});tx.commit();
    db.checkpoint();CHECK(std::filesystem::file_size(file)<1024);
    expect(ErrorCode::conflict,[&]{Database::open(file);}); // replacement remains locked
 }
 for(auto stage:{"checkpoint_created","checkpoint_synced","checkpoint_renamed","checkpoint_directory_synced"}) {
    auto file=temp.path/(std::string("checkpoint-error-")+stage);std::filesystem::copy_file(large,file);
    std::optional<Database> db;db.emplace(Database::open(file));
    stop_stage=stage;crash=false;detail::storage_hook(stop);
    expect(ErrorCode::io,[&]{auto tx=db->begin();tx.erase("big");tx.commit();});detail::storage_hook(nullptr);
    expect(ErrorCode::state,[&]{db->begin();});db.reset();db.emplace(Database::open(file));
    CHECK(db->stats().rows==((std::strcmp(stage,"checkpoint_renamed")==0||std::strcmp(stage,"checkpoint_directory_synced")==0)?0:5000));
 }
 // Repeated changes automatically bound history, not just explicit checkpoints.
 auto bounded=temp.path/"bounded";
 {auto db=Database::open(bounded);auto tx=db.begin();tx.create_table("t",{{"s",text()}});tx.insert("t",{std::string(20000,'a')});tx.commit();
  for(int i=0;i<65;++i){auto change=db.begin();change.update("t",{{"s",literal(std::string(20000,static_cast<char>('a'+i%26)))}});change.commit();
    CHECK(std::filesystem::file_size(bounded)<=1024*1024);}
  CHECK(db.storage_stats().checkpoints>=1);
  auto snapshot=db.begin();db.checkpoint();CHECK(snapshot.query(Query{"t"}).rows.size()==1);snapshot.commit();}
 {auto db=Database::open(bounded);CHECK(std::get<std::string>(db.query(Query{"t"}).rows[0][0])==std::string(20000,static_cast<char>('a'+64%26)));}
 // An opener paused on the old inode must not acquire a stale database after
 // atomic replacement releases that inode's lock.
 {
    auto file=temp.path/"open-race";seed(file);
    std::optional<Database> owner;owner.emplace(Database::open(file,registry()));
    int ready[2],resume[2];CHECK(::pipe(ready)==0);CHECK(::pipe(resume)==0);
    auto pid=::fork();CHECK(pid>=0);
    if(pid==0){
      owner.reset();::close(ready[0]);::close(resume[1]);notify_fd=ready[1];resume_fd=resume[0];detail::storage_hook(race);
      try {Database::open(file,registry());::_exit(81);}catch(const Error& e){::_exit(e.code==ErrorCode::conflict?0:82);}catch(...){::_exit(83);}
    }
    ::close(ready[1]);::close(resume[0]);char byte;CHECK(::read(ready[0],&byte,1)==1);
    owner->checkpoint();CHECK(::write(resume[1],&byte,1)==1);
    int status=0;CHECK(::waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    ::close(ready[0]);::close(resume[1]);
 }
 auto malformed=temp.path/"malformed";{std::ofstream f(malformed);f<<"bad";}
 expect(ErrorCode::format,[&]{Database::open(malformed);});
 std::filesystem::create_symlink(complete,temp.path/"alias");
 expect(ErrorCode::io,[&]{Database::open(temp.path/"alias",registry());});
});}
