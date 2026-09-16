#include "coresql/sql.hpp"
#include <iostream>

// This utility supports builtin SQL types. Applications with custom add-ons should
// install their registry and use Database::backup / Database::restore directly.
int main(int argc, char** argv) {
    try {
        if (argc != 4)
            throw std::runtime_error(
                "usage: coresql_admin backup|restore|import-snapshot SOURCE NEW_DESTINATION");
        std::string operation = argv[1];
        if (operation != "backup" && operation != "restore" && operation != "import-snapshot")
            throw std::runtime_error("Unknown operation");
        if (!std::filesystem::is_regular_file(argv[2]))
            throw std::runtime_error("Source must be an existing file");
        coresql::Registry registry;
        coresql::sql::install(registry);
        if (operation == "import-snapshot") {
            auto result = coresql::Database::restore(argv[2], argv[3], registry);
            result.begin().integrity_check();
        } else {
            auto source = coresql::Database::open(argv[2], registry);
            source.begin().integrity_check();
            source.backup(argv[3]);
            auto result = coresql::Database::open(argv[3], registry);
            result.begin().integrity_check();
            if (result.schema() != source.schema() || result.stats().rows != source.stats().rows)
                throw std::runtime_error("Backup verification failed");
        }
        std::cout << "Verified persistent database: " << argv[3] << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
