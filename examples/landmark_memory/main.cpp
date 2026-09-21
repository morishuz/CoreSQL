#include "workload.hpp"
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    using namespace coresql;
    try {
        if (argc != 2)
            throw Error(ErrorCode::state, "Usage: coresql_landmark_memory NEW_DIRECTORY");
        const std::filesystem::path directory(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw Error(ErrorCode::io, "Demo requires a new directory; existing data is never overwritten");
        auto registry = landmarks::registry();
        const auto file = directory / "landmarks.core";
        constexpr std::int64_t rows = 1000, observation = 551;
        const auto expected = landmarks::oracle(rows, observation);
        {
            auto db = Database::open(file, registry);
            sql::Connection c(db, registry);
            c.execute("BEGIN");
            landmarks::create(c);
            landmarks::insert(c, 0, rows);
            c.execute("COMMIT");
            auto old = db.begin();
            c.execute("UPDATE landmarks SET seen=1 WHERE id=551");
            // A retained reader still sees the previous committed observation.
            auto previous = old.query(Query{"landmarks",
                                            {column("seen")},
                                            Predicate{column("id"), Compare::equal, literal(observation)}});
            landmarks::check(previous.rows, {{std::int64_t{0}}});
            old.rollback();
            auto result = c.execute(landmarks::search_statement(), landmarks::search_parameters(observation));
            landmarks::check(result.rows, expected);
            for (const auto& row : result.rows)
                std::cout << "candidate=" << std::get<std::int64_t>(row[0])
                          << " squared_distance=" << std::get<double>(row[1]) << '\n';
            db.checkpoint();
            db.backup(directory / "backup.core");
        }
        for (const auto& path : {file, directory / "backup.core"}) {
            auto db = Database::open(path, registry);
            sql::Connection c(db, registry);
            landmarks::check(
                c.execute(landmarks::search_statement(), landmarks::search_parameters(observation)).rows,
                expected);
            landmarks::check(c.execute("SELECT seen FROM landmarks WHERE id=551").rows, {{std::int64_t{1}}});
            db.begin().integrity_check();
        }
        std::cout << "Verified exact search, snapshot isolation, durable reopen and backup.\n"
                  << "Synthetic descriptors; candidates are not navigation decisions.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
