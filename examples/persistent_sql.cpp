#include "coresql/sql.hpp"
#include <array>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("usage: coresql_persistent_sql DATABASE");
        coresql::Registry registry;
        coresql::sql::install(registry);
        {
            auto db = coresql::Database::open(argv[1], registry);
            coresql::sql::Connection connection(db, registry);
            connection.execute(
                "CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)");
            connection.execute("BEGIN");
            try {
                const coresql::sql::Statement write("REPLACE INTO notes (id, body) VALUES (?, ?)");
                const std::array<coresql::Value, 2> parameters{std::int64_t{1},
                                                               std::string("Persistent hello")};
                connection.execute(write, parameters);
                connection.execute("COMMIT");
            } catch (...) {
                // An I/O error may mean the commit outcome is uncertain. End this
                // connection and reopen before deciding whether to retry the write.
                if (connection.in_transaction())
                    connection.execute("ROLLBACK");
                throw;
            }
        }
        auto reopened = coresql::Database::open(argv[1], registry);
        coresql::sql::Connection connection(reopened, registry);
        auto result = connection.execute("SELECT id, body FROM notes WHERE id = 1");
        if (result.rows.size() != 1 || result.rows[0][1] != coresql::Value(std::string("Persistent hello")))
            throw std::runtime_error("Reopen verification failed");
        std::cout << result.columns[1] << ": " << std::get<std::string>(result.rows[0][1]) << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
