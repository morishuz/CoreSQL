#include <coresql/core.hpp>
int main() {
    coresql::Database db;
    auto tx = db.begin();
    tx.create_table("items", {{"id", coresql::integer()}});
    tx.insert("items", {std::int64_t{7}});
    tx.commit();
    return db.query({"items", {}}).rows.at(0).at(0) == coresql::Value(std::int64_t{7}) ? 0 : 1;
}
