#include <coresql/sql.hpp>
#include <coresql/date.hpp>
#include <coresql/decimal.hpp>
#include <coresql/vector.hpp>
#include <coresql/timestamp.hpp>
#include <coresql/spatial.hpp>
#include <coresql/json.hpp>
#include <coresql/graph.hpp>
int main() {
    coresql::Registry registry;
    coresql::sql::install(registry);
    coresql::vectors::install(registry);
    coresql::timestamps::install(registry);
    coresql::spatial::install(registry);
    coresql::json::install(registry);
    coresql::graph::install(registry);
    coresql::Database db(registry);
    coresql::sql::Connection sql(db, registry);
    if (coresql::decimals::format(
            sql.execute("SELECT CAST(0.1 AS DECIMAL(15,2))+CAST(0.2 AS DECIMAL(15,2))").rows.at(0).at(0)) !=
        "0.30")
        return 1;
    if (sql.execute("WITH x AS (SELECT 7 AS n) SELECT n FROM (SELECT n FROM x) AS d").rows.at(0).at(0) !=
        coresql::Value(std::int64_t{7}))
        return 1;
    return coresql::dates::format(sql.execute("SELECT DATE '2000-02-29'").rows.at(0).at(0)) == "2000-02-29"
               ? 0
               : 1;
}
