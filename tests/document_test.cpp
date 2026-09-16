#include "check.hpp"
#include "../examples/document_index.hpp"
#include "../src/storage.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <sys/wait.h>
using namespace coresql;
namespace {
struct Document {std::int64_t id,time;std::string text;std::array<float,4> vector;};
Registry types(){Registry r;vectors::install(r);timestamps::install(r);return r;}
const char* stage=nullptr;
void crash(const char* current){if(std::strcmp(current,stage)==0)::_exit(77);}
}
int main(){return tests([]{
    TempDirectory temp;auto path=temp.path/"documents";
    expect(ErrorCode::io,[&]{example::DocumentIndex absent(path);});CHECK(!std::filesystem::exists(path));
    {example::DocumentIndex docs(path,4);CHECK(docs.dimensions()==4);CHECK(docs.list().rows.empty());}
    std::vector<Document> model;
    // Seed several storage chunks in one transaction, then exercise application
    // update-or-insert and query APIs against an independent reference model.
    {auto db=Database::open(path,types());auto t=db.begin();
     for(std::int64_t i=0;i<300;++i){Document d{i,i%7,"document "+std::to_string(i),{float(i%5),0.F,1.F,2.F}};
        model.push_back(d);t.insert("documents",{d.id,d.text,vectors::value(d.vector,4),timestamps::value(d.time)});}t.commit();}
    std::optional<example::DocumentIndex> docs;docs.emplace(path);
    const std::array<float,4> query{1.F,0.F,1.F,2.F};
    auto verify=[&] {
        auto recent=model;std::stable_sort(recent.begin(),recent.end(),[](const auto& a,const auto& b){return a.time>b.time;});
        auto rows=docs->list(1000).rows;CHECK(rows.size()==model.size());
        for(std::size_t i=0;i<rows.size();++i){CHECK(std::get<std::int64_t>(rows[i][0])==recent[i].id);CHECK(std::get<std::string>(rows[i][1])==recent[i].text);CHECK(timestamps::microseconds(rows[i][2])==recent[i].time);}
        auto distance=[&](const auto& d){double sum=0;for(std::size_t i=0;i<4;++i){double delta=double(d.vector[i])-query[i];sum+=delta*delta;}return sum;};
        auto nearest=model;std::erase_if(nearest,[](const auto& d){return d.time<=2;});
        std::stable_sort(nearest.begin(),nearest.end(),[&](const auto& a,const auto& b){return distance(a)<distance(b);});
        auto matches=docs->nearest(query,10,2).rows;CHECK(matches.size()==std::min(std::size_t{10},nearest.size()));
        for(std::size_t i=0;i<matches.size();++i){CHECK(std::get<std::int64_t>(matches[i][0])==nearest[i].id);CHECK(std::get<double>(matches[i][2])==distance(nearest[i]));}
    };
    verify();
    auto bounded=docs->nearest(query,1000,2,4,0.0).rows;
    std::size_t bounded_count=0;
    for(const auto& d:model)if(d.time>2&&d.time<=4&&d.vector==query)++bounded_count;
    CHECK(bounded.size()==bounded_count);
    for(const auto& row:bounded){CHECK(std::get<double>(row[2])==0);CHECK(timestamps::microseconds(row[3])>2&&timestamps::microseconds(row[3])<=4);}
    CHECK(docs->list(1000,4,2).rows.empty());
    expect(ErrorCode::type,[&]{docs->nearest(query,0,{}, {},std::numeric_limits<double>::infinity());});
    std::mt19937 random(7);
    for(int step=0;step<100;++step){
        auto id=static_cast<std::int64_t>(random()%330);auto found=std::find_if(model.begin(),model.end(),[&](const auto& d){return d.id==id;});
        if(step%4==0){CHECK(docs->erase(id)==(found!=model.end()));if(found!=model.end())model.erase(found);}
        else {Document d{id,static_cast<std::int64_t>(random()%13),"revision "+std::to_string(step),{float(random()%5),float(random()%3),1.F,2.F}};
            CHECK(docs->put(d.id,d.text,d.time,d.vector)==(found==model.end()));if(found==model.end())model.push_back(d);else *found=d;}
        verify();
        if(step%10==0){docs.reset();{auto db=Database::open(path,types());db.checkpoint();}docs.emplace(path);verify();}
    }
    expect(ErrorCode::type,[&]{docs->put(999,"invalid",0,std::array{1.F,2.F});});CHECK(docs->get(999).rows.empty());
    expect(ErrorCode::type,[&]{docs->nearest(std::array{1.F,2.F},0);});verify();docs.reset();
    expect(ErrorCode::schema,[&]{example::DocumentIndex wrong(path,3);});
    // Whole application updates survive or disappear together when the process
    // exits during a commit. Both vector and timestamp payloads are checked.
    for(auto stop:{"half_payload","footer"}) {
        auto file=temp.path/stop;std::filesystem::copy_file(path,file);
        pid_t pid=::fork();CHECK(pid>=0);
        if(pid==0){try{example::DocumentIndex app(file);stage=stop;detail::storage_hook(crash);app.put(999,"atomic",999,std::array{2.F,3.F,4.F,5.F});::_exit(78);}catch(...){::_exit(79);}}
        int status;CHECK(::waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==77);
        example::DocumentIndex app(file);auto result=app.get(999);
        CHECK(result.rows.size()==(std::strcmp(stop,"footer")==0?1:0));
        if(!result.rows.empty()){CHECK(std::get<std::string>(result.rows[0][1])=="atomic");CHECK(timestamps::microseconds(result.rows[0][2])==999);
            CHECK(std::get<double>(app.nearest(std::array{2.F,3.F,4.F,5.F},1,998).rows.at(0)[2])==0);}
    }
    // Direct engine writes obey the same constraint as application writes.
    {auto db=Database::open(path,types());auto t=db.begin();
        Row row{std::int64_t{999},std::string("unique"),vectors::value(query,4),timestamps::value(0)};
        t.insert("documents",row);
        expect(ErrorCode::constraint,[&]{t.insert("documents",row);});t.commit();}
    docs.emplace(path);CHECK(docs->get(999).rows.size()==1);
    CHECK(!docs->put(999,"updated",1,query));CHECK(docs->erase(999));docs.reset();
});}
