#include "../examples/landmark_memory/workload.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>

namespace {
using namespace coresql;
using Clock = std::chrono::steady_clock;
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        auto pattern = (std::filesystem::temp_directory_path() / "coresql-operational-XXXXXX").string();
        auto* created = mkdtemp(pattern.data());
        if (!created)
            throw Error(ErrorCode::io, "Cannot create benchmark directory");
        path = created;
    }
    ~Temporary() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
std::uint64_t peak() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage))
        throw Error(ErrorCode::state, "getrusage failed");
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}
template <class Operation, class Verify>
void measure(const char* name, std::int64_t rows, int repeats, const std::filesystem::path& file,
             Operation operation, Verify verify) {
    for (int trial = 0; trial < repeats; ++trial) {
        const auto start = Clock::now();
        operation(trial);
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto rss = peak();
        const auto bytes = std::filesystem::file_size(file);
        verify(trial); // Full correctness checks never enter measured intervals.
        std::cout << name << ',' << rows << ',' << trial << ',' << std::setprecision(10) << ms << ',' << rss
                  << ',' << bytes << '\n';
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw Error(ErrorCode::state, "Usage: coresql_operational_profile rows repetitions");
        const auto n = std::stoll(argv[1]);
        const auto repeats = std::stoi(argv[2]);
        if (n < 1000 || n > 100000 || repeats < 3 || repeats > 100)
            throw Error(ErrorCode::constraint, "rows 1000..100000; repetitions 3..100");
        Temporary temp;
        const auto file = temp.path / "memory";
        const auto registry = landmarks::registry();
        std::optional<Database> db;
        std::unique_ptr<sql::Connection> c;
        auto open = [&] {
            db.emplace(Database::open(file, registry));
            c = std::make_unique<sql::Connection>(*db, registry);
        };
        auto close = [&] {
            c.reset();
            db.reset();
        };
        auto verify_count = [&](std::int64_t count) {
            landmarks::check(c->execute("SELECT count(*) FROM landmarks").rows, {{count}});
        };
        open();
        std::cout << "operation,rows,trial,milliseconds,process_peak_rss_bytes,file_bytes\n";
        measure(
            "initial_load_commit", n, 1, file,
            [&](int) {
                c->execute("BEGIN");
                landmarks::create(*c);
                landmarks::insert(*c, 0, n);
                c->execute("COMMIT");
            },
            [&](int) {
                verify_count(n);
                db->begin().integrity_check();
            });
        sql::Result answer;
        const auto search = landmarks::search_statement();
        const auto params = landmarks::search_parameters(551);
        const auto expected = landmarks::oracle(n, 551);
        c->execute(search, params); // Read-only warmup; mutation operations have no hidden warmup.
        measure(
            "vector_search", n, repeats, file, [&](int) { answer = c->execute(search, params); },
            [&](int) { landmarks::check(answer.rows, expected); });
        sql::Statement point("SELECT seen FROM landmarks WHERE id=?"),
            update("UPDATE landmarks SET seen=? WHERE id=?");
        measure(
            "prepared_point_read", n, repeats, file,
            [&](int) { answer = c->execute(point, std::array<Value, 1>{std::int64_t{551}}); },
            [&](int) { landmarks::check(answer.rows, {{std::int64_t{0}}}); });
        measure(
            "single_update_commit", n, repeats, file,
            [&](int i) { c->execute(update, std::array<Value, 2>{std::int64_t{i + 1}, std::int64_t{551}}); },
            [&](int i) {
                landmarks::check(c->execute(point, std::array<Value, 1>{std::int64_t{551}}).rows,
                                 {{std::int64_t{i + 1}}});
            });
        measure(
            "batch_32_update_commit", n, repeats, file,
            [&](int i) {
                c->execute("BEGIN");
                for (std::int64_t id = 0; id < 32; ++id)
                    c->execute(update, std::array<Value, 2>{std::int64_t{i + 1}, id});
                c->execute("COMMIT");
            },
            [&](int i) {
                landmarks::check(c->execute("SELECT sum(seen) FROM landmarks WHERE id<32").rows,
                                 {{std::int64_t{32 * (i + 1)}}});
            });
        measure(
            "mixed_transaction", n, repeats, file,
            [&](int i) {
                c->execute("BEGIN");
                c->execute(update, std::array<Value, 2>{std::int64_t{i + 1}, std::int64_t{551}});
                answer = c->execute(point, std::array<Value, 1>{std::int64_t{551}});
                landmarks::insert(*c, n, 1);
                c->execute("DELETE FROM landmarks WHERE id=?", std::array<Value, 1>{n});
                c->execute("COMMIT");
            },
            [&](int i) {
                landmarks::check(answer.rows, {{std::int64_t{i + 1}}});
                verify_count(n);
            });
        const auto old_value = c->execute(point, std::array<Value, 1>{std::int64_t{551}}).rows;
        std::vector<Transaction> snapshots;
        measure(
            "retain_snapshot_update_all", n, 4, file,
            [&](int i) {
                snapshots.push_back(db->begin());
                c->execute("UPDATE landmarks SET seen=?", std::array<Value, 1>{std::int64_t{100 + i}});
            },
            [&](int i) {
                Query q{"landmarks",
                        {column("seen")},
                        Predicate{column("id"), Compare::equal, literal(std::int64_t{551})}};
                for (int j = 0; j <= i; ++j)
                    landmarks::check(snapshots[static_cast<std::size_t>(j)].query(q).rows,
                                     j ? std::vector<Row>{{std::int64_t{99 + j}}} : old_value);
            });
        snapshots.clear();
        close();
        measure(
            "reopen", n, repeats, file, [&](int) { open(); },
            [&](int) {
                verify_count(n);
                landmarks::check(c->execute(search, params).rows, expected);
                close();
            });
        open();
        measure(
            "checkpoint", n, 3, file, [&](int) { db->checkpoint(); },
            [&](int) { db->begin().integrity_check(); });
        const auto backup = temp.path / "backup";
        measure(
            "backup", n, 1, file, [&](int) { db->backup(backup); },
            [&](int) {
                auto copy = Database::open(backup, registry);
                sql::Connection connection(copy, registry);
                landmarks::check(connection.execute(search, params).rows, expected);
                copy.begin().integrity_check();
            });
        close();
        std::cerr << "Verified all results. Synchronized local-file commits; 128-D synthetic vectors; "
                     "RSS is process high-water including earlier phases, not per-operation allocation.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
