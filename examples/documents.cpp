#include "document_index.hpp"
#include <array>
#include <charconv>
#include <iomanip>
#include <iostream>

namespace {
std::int64_t number(std::string_view text) {
    std::int64_t result;
    auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),result);
    if(error!=std::errc{}||end!=text.data()+text.size())throw std::runtime_error("Expected an integer: "+std::string(text));
    return result;
}
std::size_t limit(std::string_view text) {
    auto n=number(text);if(n<0)throw std::runtime_error("Limit must be nonnegative");return static_cast<std::size_t>(n);
}
std::vector<float> vector(std::string_view text) {
    std::vector<float> result;
    while(true) {
        auto comma=text.find(',');auto part=text.substr(0,comma);float value;
        auto [end,error]=std::from_chars(part.data(),part.data()+part.size(),value);
        if(error!=std::errc{}||end!=part.data()+part.size())throw std::runtime_error("Expected comma-separated finite floats");
        result.push_back(value);
        if(comma==text.npos)return result;
        text.remove_prefix(comma+1);
    }
}
void print(const coresql::Result& result,bool distances=false) {
    for(const auto& row:result.rows) {
        std::cout<<std::get<std::int64_t>(row[0])<<'\t'<<std::quoted(std::get<std::string>(row[1]));
        if(distances)std::cout<<"\tdistance_squared="<<std::get<double>(row[2]);
        std::cout<<"\tupdated_us="<<coresql::timestamps::microseconds(row.back())<<'\n';
    }
    if(result.rows.empty())std::cout<<"No matching documents\n";
}
void demo(const std::filesystem::path& path) {
    if(std::filesystem::exists(path))throw std::runtime_error("Demo requires a new database path");
    {
        example::DocumentIndex docs(path,3);
        docs.put(1,"Storage journal notes",100,std::array{0.95F,0.05F,0.F});
        docs.put(2,"Geometry notes",200,std::array{0.05F,0.95F,0.F});
        docs.put(3,"Recovery checklist",300,std::array{0.8F,0.2F,0.F});
        std::cout<<"Inserted three documents with illustrative, hand-authored vectors.\nNearest to [1,0,0]:\n";
        print(docs.nearest(std::array{1.F,0.F,0.F},2),true);
        std::cout<<"Modified after 150 microseconds since Unix epoch:\n";print(docs.list(20,150));
        docs.put(3,"Revised recovery checklist",400,std::array{0.9F,0.1F,0.F});docs.erase(2);
    }
    example::DocumentIndex reopened(path);
    auto rows=reopened.list();
    if(rows.rows.size()!=2||std::get<std::string>(reopened.get(3).rows.at(0)[1])!="Revised recovery checklist")
        throw std::runtime_error("Reopen verification failed");
    std::cout<<"Reopened after update and deletion:\n";print(rows);
    std::cout<<"Database retained at "<<path<<'\n';
}
void usage() {
    std::cerr<<"Usage: coresql_documents DATABASE command [arguments]\n"
        "  demo                              Run a walkthrough in a new database\n"
        "  init DIMENSIONS                   Initialize or verify a document database\n"
        "  put ID UPDATED_US VECTOR TEXT     Insert or replace a document\n"
        "  get ID | delete ID                Retrieve or delete a document\n"
        "  list [LIMIT [SINCE_US]]            Newest first; updated strictly after SINCE\n"
        "  nearest VECTOR [LIMIT [SINCE_US]]  Exact squared-L2 search\n"
        "  info                              Show the stored embedding dimension\n"
        "VECTOR is comma-separated, e.g. 0.1,0.2,0.3. Quote TEXT in the shell.\n";
}
}
int main(int argc,char** argv) {
    try {
        if(argc<3){usage();return 1;}
        std::filesystem::path path=argv[1];std::string command=argv[2];
        if(command=="demo"&&argc==3){demo(path);return 0;}
        if(command=="init"&&argc==4){auto n=number(argv[3]);if(n<=0)throw std::runtime_error("Dimension must be positive");example::DocumentIndex docs(path,static_cast<std::uint64_t>(n));std::cout<<"Ready: "<<docs.dimensions()<<" dimensions\n";return 0;}
        // Validate command shape before opening/creating any files.
        if(!((command=="put"&&argc==7)||((command=="get"||command=="delete")&&argc==4)||
             (command=="list"&&argc>=3&&argc<=5)||(command=="nearest"&&argc>=4&&argc<=6)||(command=="info"&&argc==3))) {usage();return 1;}
        example::DocumentIndex docs(path);
        if(command=="put")std::cout<<(docs.put(number(argv[3]),argv[6],number(argv[4]),vector(argv[5]))?"Inserted\n":"Updated\n");
        else if(command=="get")print(docs.get(number(argv[3])));
        else if(command=="delete")std::cout<<(docs.erase(number(argv[3]))?"Deleted\n":"Not found\n");
        else if(command=="info")std::cout<<docs.dimensions()<<" embedding dimensions; exact squared-L2 search\n";
        else if(command=="list")print(docs.list(argc>=4?limit(argv[3]):20,argc==5?std::optional{number(argv[4])}:std::nullopt));
        else print(docs.nearest(vector(argv[3]),argc>=5?limit(argv[4]):10,argc==6?std::optional{number(argv[5])}:std::nullopt),true);
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
