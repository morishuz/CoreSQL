// Private test protocol: decimal byte count + SQL, then tab-separated hex cells.
#include "coresql/sql.hpp"
#include "coresql/date.hpp"
#include "coresql/decimal.hpp"
#include <iostream>
#include <cstdlib>
#include "../../src/state.hpp"
#include <iomanip>
#include <sstream>
#include <limits>
#ifdef CORESQL_ALLOCATION_PROFILE
#include "../../benchmarks/allocation_profile.hpp"
#endif
using namespace coresql;
std::string hex(const std::string& s) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
int main() {
    Registry registry;
    sql::install(registry);
    Database db(registry);
    sql::Connection connection(db, registry);
    std::size_t length;
    while (std::cin >> length) {
        if (length > 1024 * 1024 || std::cin.get() != '\n')
            return 2;
        std::string source(length, '\0');
        if (!std::cin.read(source.data(), static_cast<std::streamsize>(length)))
            return 2;
        detail::QueryCounters counters;
        const auto* profile_query = std::getenv("CORESQL_PROFILE_QUERY");
        const bool profile =
            std::getenv("CORESQL_PROFILE") != nullptr || (profile_query && source == profile_query);
        auto scope = profile ? std::make_unique<detail::QueryCounterScope>(counters) : nullptr;
#ifdef CORESQL_ALLOCATION_PROFILE
        const bool allocations = profile_query && source == profile_query;
        if (allocations)
            allocation_profile::begin();
#endif
        try {
            auto result = connection.execute(source);
            std::ostringstream response;
            response << "ok " << result.rows.size() << '\n';
            for (const auto& row : result.rows) {
                for (std::size_t i = 0; i < row.size(); ++i) {
                    if (i)
                        response << '\t';
                    const auto& v = row[i];
                    if (is_null(v))
                        response << 'N';
                    else if (auto n = std::get_if<std::int64_t>(&v))
                        response << 'I' << *n;
                    else if (auto n = std::get_if<double>(&v))
                        response << 'R' << std::setprecision(std::numeric_limits<double>::max_digits10) << *n;
                    else if (auto s = std::get_if<std::string>(&v))
                        response << 'T' << hex(*s);
                    else if (type_of(v) == dates::type())
                        response << 'T' << hex(dates::format(v));
                    else if (decimals::is_decimal(type_of(v)))
                        response << 'D' << decimals::format(v);
                    else
                        throw std::runtime_error("Unexpected opaque SQL result");
                }
                response << '\n';
            }
            std::cout << response.str() << std::flush;
        } catch (const Error& e) {
            std::cout << (e.code == ErrorCode::unsupported ? "unsupported " : "error ") << hex(e.what())
                      << '\n'
                      << std::flush;
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            return 2;
        }
#ifdef CORESQL_ALLOCATION_PROFILE
        if (allocations)
            allocation_profile::end();
#endif
        if (profile) {
            std::cerr << "{\"rows_tested\":" << counters.rows_tested
                      << ",\"candidate_pairs\":" << counters.candidate_pairs
                      << ",\"intermediate_rows\":" << counters.intermediate_rows
                      << ",\"largest_intermediate\":" << counters.largest_intermediate
                      << ",\"subquery_executions\":" << counters.subquery_executions
                      << ",\"hash_build_rows\":" << counters.hash_build_rows
                      << ",\"hash_probes\":" << counters.hash_probes
                      << ",\"correlation_index_rows\":" << counters.correlation_index_rows
                      << ",\"subquery_cache_hits\":" << counters.subquery_cache_hits << "}\n";
            for (const auto& stage : counters.stages)
                std::cerr << "{\"stage\":\"" << stage.kind << "\",\"rows\":" << stage.rows
                          << ",\"columns\":" << stage.columns
                          << ",\"row_capacity_bytes\":" << stage.row_capacity_bytes
                          << ",\"match_capacity_bytes\":" << stage.match_capacity_bytes << "}\n";
        }
    }
}
