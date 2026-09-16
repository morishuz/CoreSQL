#include "check.hpp"
#include "coresql/sql.hpp"
using namespace coresql;
int main() {
    return tests([] {
        Registry r;
        sql::install(r);
        Database db(r);
        sql::Connection c(db, r);
        c.execute("CREATE TABLE keys(k INTEGER)");
        c.execute("INSERT INTO keys VALUES(NULL),(1),(1),(-1),(2),(3),(4),(5),(6),(7),(8),(9),(10)");
        auto rows =
            c.execute(
                 "SELECT 1 IN (SELECT k FROM keys),99 IN (SELECT k FROM keys),NULL IN (SELECT k FROM keys)")
                .rows;
        CHECK(rows[0][0] == Value(std::int64_t{1}) && is_null(rows[0][1]) && is_null(rows[0][2]));
        CHECK(
            c.execute("SELECT NULL IN (SELECT k FROM keys WHERE 0),1 IN (SELECT k FROM keys WHERE 0)").rows ==
            std::vector<Row>({{std::int64_t{0}, std::int64_t{0}}}));
        CHECK(c.execute("SELECT -1 IN (1,2,3,4,5,6,7,8,9,-1),99 IN (1,2,3,4,5,6,7,8,9,-1)").rows ==
              std::vector<Row>({{std::int64_t{1}, std::int64_t{0}}}));
        CHECK(c.execute("SELECT '01' IN (1),1 IN ('01'), 'one' IN ('two','one')").rows ==
              std::vector<Row>({{std::int64_t{0}, std::int64_t{0}, std::int64_t{1}}}));
        // A cached lookup must not survive an update and another execution.
        c.execute("DELETE FROM keys");
        CHECK(c.execute("SELECT 1 IN (SELECT k FROM keys)").rows[0][0] == Value(std::int64_t{0}));
        // A provider which has not promised total comparison retains short-circuit errors.
        Registry custom(false), defaults;
        auto integers = defaults.addon(integer());
        integers.canonical_scalar = false;
        integers.equal = [](ByteView, const Value& a, const Value& b) {
            if (std::get<std::int64_t>(b) == 99)
                throw Error(ErrorCode::constraint, "comparison failure");
            return a == b;
        };
        integers.hash = [](ByteView, const Value&) -> std::size_t {
            throw Error(ErrorCode::state, "hash called");
        };
        custom.add(std::move(integers));
        custom.add(defaults.addon(real()));
        custom.add(defaults.addon(text()));
        install_aggregate_functions(custom);
        install_relational_functions(custom);
        sql::install(custom);
        Database other(custom);
        sql::Connection d(other, custom);
        CHECK(d.execute("SELECT 1 IN (1,99)").rows[0][0] == Value(std::int64_t{1}));
        expect(ErrorCode::constraint, [&] { d.execute("SELECT 0 IN (1,99)"); });
        CHECK(is_null(d.execute("SELECT NULL IN (99)").rows[0][0]));
        CHECK(c.execute("SELECT 'j' IN ('a','b','c','d','e','f','g','h','i','j'),"
                        "'z' IN ('a','b','c','d','e','f','g','h','i','j',NULL)")
                  .rows[0][0] == Value(std::int64_t{1}));
        CHECK(is_null(c.execute("SELECT 'z' IN ('a','b','c','d','e','f','g','h','i','j',NULL)").rows[0][0]));
        Registry truth(false);
        auto truth_type = defaults.addon(integer());
        truth_type.canonical_scalar = false;
        truth_type.validate_value = [](ByteView, const Value& v) {
            if (v == Value(std::int64_t{1}))
                throw Error(ErrorCode::constraint, "truth validation");
        };
        truth.add(std::move(truth_type));
        truth.add(defaults.addon(real()));
        truth.add(defaults.addon(text()));
        install_aggregate_functions(truth);
        install_relational_functions(truth);
        sql::install(truth);
        Database truth_db(truth);
        sql::Connection truth_sql(truth_db, truth);
        expect(ErrorCode::constraint, [&] { truth_sql.execute("SELECT 'a' IN ('a','b')"); });
    });
}
