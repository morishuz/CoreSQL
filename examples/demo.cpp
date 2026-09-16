#include "coresql/core.hpp"
#include "coresql/vector.hpp"
#include "coresql/timestamp.hpp"
#include <array>
#include <iostream>

int main(int argc, char** argv) {
    try {
        using namespace coresql;
        Registry registry;
        vectors::install(registry);
        timestamps::install(registry);
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("documents", {
            {"title", text()}, {"embedding", vectors::type()}, {"created", timestamps::type()}
        });
        tx.insert("documents", {std::string("Storage design"),
            vectors::value(std::array{1.0F, 2.0F, 3.0F}), timestamps::value(1'000'000)});
        tx.insert("documents", {std::string("Geometry notes"),
            vectors::value(std::array{4.0F, 5.0F, 6.0F}), timestamps::value(2'000'000)});
        tx.commit();

        auto distance = call("vector.squared_l2", {
            column("embedding"), literal(vectors::value(std::array{1.0F, 2.0F, 3.0F}))
        });
        Query nearest{"documents", {column("title"), distance}, {}, {Order{distance}}, 2};
        for (const auto& row : db.query(nearest).rows)
            std::cout << std::get<std::string>(row[0]) << ": distance=" << std::get<double>(row[1]) << '\n';
        auto stats = db.stats();
        std::cout << stats.rows << " rows, " << stats.stored_payload_bytes << " stored payload bytes\n";

        // Optional explicit snapshot export to a NEW path (no overwrite).
        if (argc == 2) {
            db.save(argv[1]);
            auto reopened = Database::load(argv[1], registry);
            std::cout << "Reopened " << reopened.stats().rows << " rows\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
