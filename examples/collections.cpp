#include "coresql/core.hpp"
#include <iostream>
using namespace coresql;
int main() {
    Database db;
    auto tx=db.begin();
    tx.create_table("collections",{{"id",integer(),true},{"name",text()}});
    tx.create_table("documents",{{"id",integer(),true},{"collection",integer()},{"title",text()}});
    tx.insert("collections",{std::int64_t{1},std::string("Research")});
    tx.insert("collections",{std::int64_t{2},std::string("Personal")});
    tx.insert("documents",{std::int64_t{10},std::int64_t{1},std::string("Database notes")});
    tx.insert("documents",{std::int64_t{11},std::int64_t{2},std::string("Reading list")});
    tx.insert("documents",{std::int64_t{12},std::int64_t{99},std::string("Unassigned")});
    tx.commit();
    Query q{"documents",{column("d","title"),column("c","name")},{},{Order{column("d","id")}}};
    q.alias="d";
    q.join=InnerJoin{"collections","c",column("d","collection"),column("c","id")};
    auto result=db.query(q);
    if(result.rows.size()!=2)return 1;
    for(const auto& row:result.rows)std::cout<<std::get<std::string>(row[0])<<" — "<<std::get<std::string>(row[1])<<'\n';
}
