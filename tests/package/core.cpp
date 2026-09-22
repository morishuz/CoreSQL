#include <coresql/core.hpp>
int main() {
    coresql::Database db({}, {.concurrent_reads = true, .page_cache_bytes = 4096});
    auto tx = db.begin();
    tx.create_table("items", {{"id", coresql::integer()}});
    tx.insert("items", {std::int64_t{7}});
    tx.commit();
    auto snapshot = db.snapshot();
    auto cursor = snapshot.cursor({"items", {}});
    auto row = cursor.next();
    db.trim_cache();
    return row && row->at(0) == coresql::Value(std::int64_t{7}) && !cursor.next() ? 0 : 1;
}
