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
        integers.native_ops = false;
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
        truth_type.native_ops = false;
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

        Type measure{"test.measure", 1, {std::byte{7}}};
        TypeAddon unit;
        unit.id = measure.id;
        unit.layout = Layout::i64;
        unit.native_ops = true;
        auto require_unit = [](ByteView p) {
            if (p.size() != 1 || p[0] != std::byte{7})
                throw Error(ErrorCode::type, "missing parameters");
        };
        unit.validate_type = require_unit;
        unit.validate_value = [require_unit](ByteView p, const Value& v) {
            require_unit(p);
            (void)i64_payload(v);
        };
        unit.equal = [require_unit](ByteView p, const Value& a, const Value& b) {
            require_unit(p);
            return i64_payload(a) == i64_payload(b);
        };
        unit.compare = [require_unit](ByteView p, const Value& a, const Value& b) {
            require_unit(p);
            auto x = i64_payload(a), y = i64_payload(b);
            return (x > y) - (x < y);
        };
        unit.hash = [require_unit](ByteView p, const Value& v) {
            require_unit(p);
            return std::hash<std::int64_t>{}(i64_payload(v));
        };
        Registry measures;
        measures.add(std::move(unit));
        sql::install(measures);
        auto in = [&](std::int64_t needle, std::vector<std::int64_t> candidates) {
            std::vector<Expr> values;
            values.reserve(candidates.size());
            for (auto n : candidates)
                values.push_back(literal(compact(measure, n)));
            Query q{"", {membership(literal(compact(measure, needle)), std::move(values), "sql.equal")}};
            auto rows = Database(measures).query(q).rows;
            CHECK(rows.size() == 1 && rows[0].size() == 1);
            return rows[0][0];
        };
        CHECK(in(1, {1, 2}) == Value(std::int64_t{1}));
        CHECK(in(3, {1, 2}) == Value(std::int64_t{0}));
        CHECK(in(9, {1, 2, 3, 4, 5, 6, 7, 8, 9}) == Value(std::int64_t{1}));
        CHECK(in(0, {1, 2, 3, 4, 5, 6, 7, 8, 9}) == Value(std::int64_t{0}));
    });
}
