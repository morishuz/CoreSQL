#include "coresql/core.hpp"
#include "coresql/vector.hpp"
#include <chrono>
#include <iostream>
#include <random>
using namespace coresql;
int main(int argc,char** argv) {
 try {
    if(argc!=4)throw std::runtime_error("Usage: coresql_query_profile scan|filter|sort|top10|vector_top10|text_filter|text_sort|text_top10 rows repeats");
    std::string mode=argv[1];auto n=std::stoull(argv[2]),repeats=std::stoull(argv[3]);
    if(n<10||n>1000000||!repeats||repeats>100000)throw std::runtime_error("Invalid workload size");
    Registry r;vectors::install(r);Database db(r);auto tx=db.begin();
    const bool vector=mode=="vector_top10";
    const bool long_text=mode.starts_with("text_");
    tx.create_table("t",{{"id",integer()},{"key",integer()},{"data",vector?vectors::type(32):text()}});
    std::mt19937 random(42);
    for(std::size_t i=0;i<n;++i) {
        Value data=std::string(long_text?4096:64,'x');
        if(long_text){auto prefix=std::to_string((i*7919)%n);std::get<std::string>(data).replace(0,prefix.size(),prefix);}
        if(vector){std::vector<float> v(32);for(auto& x:v)x=float(random()%100)/100;data=vectors::value(v,32);}
        tx.insert("t",{static_cast<std::int64_t>(i),static_cast<std::int64_t>(random()%1000),std::move(data)});
    }
    tx.commit();Query query{"t",{column("id")}};
    if(mode=="text_filter") {auto text=std::string(4096,'x');text[0]='0';query.where=Predicate{column("data"),Compare::equal,literal(std::move(text))};}
    else if(mode=="text_sort"||mode=="text_top10"){query.order_by={Order{column("data")}};if(mode=="text_top10")query.limit=10;}
    else if(mode=="filter")query.where=Predicate{column("key"),Compare::equal,literal(std::int64_t{50})};
    else if(mode=="sort"||mode=="top10") {query.order_by={Order{column("key")}};if(mode=="top10")query.limit=10;}
    else if(vector){query.order_by={Order{call("vector.squared_l2",{column("data"),literal(vectors::value(std::vector<float>(32,0.5F),32))})}};query.limit=10;}
    else if(mode!="scan")throw std::runtime_error("Unknown workload");
    auto expected=db.query(query).rows;auto start=std::chrono::steady_clock::now();std::size_t rows=0;
    for(std::size_t i=0;i<repeats;++i){auto result=db.query(query);if(result.rows.size()!=expected.size())throw std::runtime_error("Unexpected result count");rows+=result.rows.size();}
    auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<mode<<','<<n<<','<<repeats<<','<<elapsed/repeats<<','<<rows<<'\n';
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
