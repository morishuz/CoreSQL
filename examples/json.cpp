#include "coresql/json.hpp"
#include <iostream>

int main() {
    using namespace coresql;
    Registry registry;
    json::install(registry);
    registry.add(json::property("documents.status.v1", "/status"));
    Database db(registry);
    auto tx = db.begin();
    tx.create_table("documents", {
        {"id", integer(), true},
        {"body", json::type(), false, "documents.status.v1"}
    });
    tx.insert("documents", {std::int64_t{1}, json::value(R"({"status":"ready","tags":["robotics"]})")});
    tx.insert("documents", {std::int64_t{2}, json::value(R"({"status":"pending"})")});
    tx.commit();
    // Removing the index name above changes the access path, not this query.
    auto result = db.query({"documents", {column("id")},
        contains(column("body"), "documents.status.v1", literal(std::string("ready")))});
    for (const auto& row : result.rows) std::cout << "Ready document: " << std::get<std::int64_t>(row[0]) << '\n';
}
