#include "type.hpp"
#include "sql.hpp"
#include "coresql/sql.hpp"
#include <iostream>

using namespace coresql;
namespace codes = example::codes;
int main(int argc, char** argv) {
    try {
        // Backend-only use requires type.cpp and CoreSQL::core, with no SQL dependency.
        Registry backend;
        codes::install(backend);
        Database memory(backend);
        auto tx = memory.begin();
        tx.create_table("codes", {{"code", codes::type()}});
        tx.insert("codes", {codes::value("AB")});
        tx.commit();
        if (codes::string(memory.query(Query{"codes", {column("code")}}).rows.at(0).at(0)) != "AB")
            return 1;

        // SQL installs the backend through the adapter: do not install it twice.
        auto types = sql::default_type_adapters();
        types.add(codes::sql_adapter());
        Registry registry;
        sql::install(registry, types);
        auto db = argc > 1 ? Database::open(argv[1], registry) : Database(registry);
        sql::Connection connection(db, registry, types);
        connection.execute("CREATE TABLE IF NOT EXISTS labels(id INTEGER PRIMARY KEY, code CODE)");
        sql::Statement insert("INSERT OR REPLACE INTO labels VALUES(?, CAST(? AS CODE))", types);
        connection.execute(insert, Row{std::int64_t{1}, std::string("AB")});
        connection.execute("INSERT OR REPLACE INTO labels VALUES(2, CODE 'CD')");
        auto result =
            connection.execute("SELECT CAST(code AS TEXT), code_first_letter(code) FROM labels ORDER BY id");
        if (result.rows !=
            std::vector<Row>{{std::string("AB"), std::string("A")}, {std::string("CD"), std::string("C")}})
            return 1;
        if (!is_null(connection.execute("SELECT CAST(NULL AS CODE)").rows.at(0).at(0)))
            return 1;
        for (const auto& row : result.rows)
            std::cout << std::get<std::string>(row[0]) << ' ' << std::get<std::string>(row[1]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
