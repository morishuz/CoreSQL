#include "speedtest1_support.hpp"
constexpr std::array ids{100,110,120,142,145};
struct Run {
    std::array<double, ids.size()> ms{};
    std::array<Rows, 3> tables;
    std::array<std::vector<Rows>, 2> queries;
};
template<class F> double timed(F&& f) {
    auto start = Clock::now(); f();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
Run run(unsigned size, bool core_engine) {
    Run out;
    Database core(registry()); SQLite sql;
    const unsigned n = size * 500, mask = roundup_allones(n);
    for (int test = 0; test < 3; ++test) {
        const std::string table = test == 0 ? "z1" : test == 1 ? "z2" : "t3";
        out.ms[test] = timed([&] {
            if (core_engine) {
                auto tx = core.begin();
                tx.create_table(table, {{"a", integer(), test != 0}, {"b", integer()}, {"c", text()}});
                for (unsigned i = 1; i <= n; ++i) tx.insert(table, input(i, mask, test == 1));
                tx.commit();
            } else {
                sql.exec("BEGIN");
                // Match upstream's default UNIQUE, not its optional INTEGER PRIMARY KEY.
                sql.exec("CREATE TABLE " + table + "(a INTEGER" + (test ? " UNIQUE" : "") + ",b INTEGER,c TEXT)");
                Statement insert(sql, "INSERT INTO " + table + " VALUES(?1,?2,?3)");
                for (unsigned i = 1; i <= n; ++i) {
                    auto row = input(i, mask, test == 1);
                    for (int j = 0; j < 3; ++j) insert.bind(j + 1, row[j]);
                    require(insert.step() == SQLITE_DONE, "insert returned rows"); insert.reset();
                }
                sql.exec("COMMIT");
            }
        });
        // Independent post-commit comparison, outside timing.
        if (core_engine) out.tables[test] = core.query(Query{table, {}, {}, {Order{column("a")}}}).rows;
        else { Statement scan(sql, "SELECT a,b,c FROM " + table + " ORDER BY a"); out.tables[test] = scan.rows(); }
    }
    for (int test = 0; test < 2; ++test) {
        auto& results = out.queries[test]; results.reserve(10);
        out.ms[test + 3] = timed([&] {
            if (core_engine) {
                auto tx = core.begin();
                for (unsigned i = 1; i <= 10; ++i) {
                    auto p = pattern(i);
                    auto predicate = Predicate{call("benchmark.ascii_contains", {column("c"), literal(p.substr(1, p.size()-2))}),
                                               Compare::equal, literal(std::int64_t{1})};
                    results.push_back(tx.query(Query{"z1", {}, predicate, {Order{column("a")}},
                        test ? std::size_t{10} : std::numeric_limits<std::size_t>::max()}).rows);
                }
                tx.commit();
            } else {
                sql.exec("BEGIN");
                Statement query(sql, std::string("SELECT a,b,c FROM z1 WHERE c LIKE ?1 ORDER BY a") + (test ? " LIMIT 10" : ""));
                for (unsigned i = 1; i <= 10; ++i) {
                    query.bind(1, pattern(i)); results.push_back(query.rows());
                }
                sql.exec("COMMIT");
            }
        });
    }
    return out;
}
int main(int argc, char** argv) {
    try {
        require(argc <= 3, "usage: coresql_speedtest1_compare [size=10] [repeats=5]");
        auto parse = [](const char* arg, unsigned maximum) {
            std::size_t end = 0; auto n = std::stoul(arg, &end);
            require(end == std::string_view(arg).size() && n >= 1 && n <= maximum, "argument out of range");
            return static_cast<unsigned>(n);
        };
        const unsigned size = argc > 1 ? parse(argv[1], 100) : 10;
        const unsigned repeats = argc > 2 ? parse(argv[2], 31) : 5;
        require(pattern(1) == "%on%" && pattern(10) == "%te%", "upstream pattern mapping changed");
        std::cerr << "SQLite " << sqlite3_libversion() << " source=" << sqlite3_sourceid()
                  << "\ncompiler=" << __VERSION__ << "; size=" << size << "; repeats=" << repeats
                  << "; fresh in-memory databases; one warmup; alternating engine order\n"
                  << "Adapted main subset: 100,110,120,142,145. No full-suite score.\n"
                  << "Includes generation, SQLite preparation, commit and owned results; validation outside timing.\n";
        std::array<std::array<std::vector<double>, ids.size()>, 2> samples;
        for (unsigned repeat = 0; repeat <= repeats; ++repeat) {
            Run a, b;
            if (repeat % 2) { b = run(size, false); a = run(size, true); }
            else { a = run(size, true); b = run(size, false); }
            for (int t = 0; t < 3; ++t) equal(a.tables[t], b.tables[t]);
            for (int t = 0; t < 2; ++t)
                for (int q = 0; q < 10; ++q) equal(a.queries[t][q], b.queries[t][q]);
            if (repeat) for (std::size_t t = 0; t < ids.size(); ++t) {
                samples[0][t].push_back(a.ms[t]); samples[1][t].push_back(b.ms[t]);
            }
        }
        std::cout << "test,size,rows,coresql_median_ms,sqlite_median_ms,coresql_min_ms,coresql_max_ms,sqlite_min_ms,sqlite_max_ms,coresql_over_sqlite\n";
        std::cout << std::fixed << std::setprecision(6);
        for (std::size_t t = 0; t < ids.size(); ++t) {
            auto& a = samples[0][t]; auto& b = samples[1][t];
            std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
            auto median = [](const auto& v) { return (v[(v.size()-1)/2] + v[v.size()/2])/2; };
            std::cout << ids[t] << ',' << size << ',' << size*500 << ',' << median(a) << ',' << median(b)
                      << ',' << a.front() << ',' << a.back() << ',' << b.front() << ',' << b.back()
                      << ',' << median(a)/median(b) << '\n';
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
