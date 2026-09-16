// Compile once per engine revision with namespace and C entry-point renaming.
#include "coresql/core.hpp"
using namespace coresql;
#define JOIN_(a,b) a##b
#define JOIN(a,b) JOIN_(a,b)
#define ENTRY(name) JOIN(PROFILE_PREFIX,name)
extern "C" void* ENTRY(_create)() {
    auto db = std::make_unique<Database>(); auto tx = db->begin();
    tx.create_table("items", {{"id", integer()}, {"score", real()}, {"name", text()}});
    for (std::int64_t i = 0; i < 100000; ++i) tx.insert("items", {i, double(i % 100), "row" + std::to_string(i)});
    tx.commit(); return db.release();
}
extern "C" void ENTRY(_update)(void* handle) {
    auto& db = *static_cast<Database*>(handle); auto edit = db.begin();
    auto count = edit.update("items", {{"name", literal(std::string("changed"))}},
        Predicate{column("id"), Compare::equal, literal(std::int64_t{50000})});
    if (count != 1) throw std::runtime_error("Update result differs");
    edit.rollback();
}
extern "C" void ENTRY(_verify)(void* handle) {
    auto& db = *static_cast<Database*>(handle);
    auto result = db.query({"items", {column("name")}, Predicate{column("id"), Compare::equal, literal(std::int64_t{50000})}});
    if (result.rows.size() != 1 || std::get<std::string>(result.rows[0][0]) != "row50000")
        throw std::runtime_error("Rollback result differs");
}
extern "C" void ENTRY(_destroy)(void* handle) { delete static_cast<Database*>(handle); }
