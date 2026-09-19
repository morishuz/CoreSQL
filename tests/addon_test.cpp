#include "check.hpp"
#include "coresql/vector.hpp"
#include "coresql/timestamp.hpp"
#include "coresql/encoding.hpp"
#include <array>
#include <cmath>

using namespace coresql;
Value vector(std::initializer_list<float> values, std::optional<std::uint64_t> dimensions = {}) {
    return vectors::value(std::span(values.begin(), values.size()), dimensions);
}
Registry addons() { Registry r; vectors::install(r); timestamps::install(r); return r; }

int main() { return tests([] {
    auto registry = addons();
    expect(ErrorCode::type, [&] { vectors::install(registry); });
    CHECK(registry.orderable(timestamps::type()));
    CHECK(!registry.orderable(vectors::type()));
    CHECK(registry.equal(vector({0, 1}), vector({-0.0F, 1})));
    CHECK(!registry.equal(vector({0}), vector({0, 1})));
    CHECK(registry.compare(timestamps::value(INT64_MIN), timestamps::value(INT64_MAX)) < 0);
    CHECK(registry.compare(timestamps::value(-1), timestamps::value(0)) < 0);
    // Antisymmetry/transitivity and equality agreement for ordered timestamps.
    std::array<std::int64_t, 5> times{INT64_MIN, -1, 0, 1, INT64_MAX};
    for (auto a : times) for (auto b : times) {
        auto x = timestamps::value(a), y = timestamps::value(b);
        CHECK(registry.compare(x, y) == -registry.compare(y, x));
        CHECK(registry.equal(x, y) == (a == b));
        for (auto c : times) if (a < b && b < c) CHECK(registry.compare(x, timestamps::value(c)) < 0);
    }

    Database db(registry);
    auto tx = db.begin();
    tx.create_table("docs", {{"name", text()}, {"embedding", vectors::type()}, {"created", timestamps::type()}});
    tx.insert("docs", {std::string("far"), vector({4, 5}), timestamps::value(30)});
    tx.insert("docs", {std::string("near"), vector({1, 2}), timestamps::value(-10)});
    tx.insert("docs", {std::string("middle"), vector({2, 3}), timestamps::value(20)});
    tx.create_table("variable", {{"v", vectors::type()}});
    tx.insert("variable", {vector({})});
    tx.insert("variable", {vector({1, 2, 3})});
    tx.insert("variable", {vector({4})});
    tx.create_table("fixed", {{"v", vectors::type(3)}});
    tx.insert("fixed", {vector({1, 2, 3}, 3)});
    tx.create_table("empty", {{"v", vectors::type()}});
    expect(ErrorCode::type, [&] { tx.insert("fixed", {Opaque(vectors::type(3), Bytes(8))}); });
    expect(ErrorCode::type, [&] { tx.insert("variable", {Opaque(vectors::type(), Bytes(3))}); });
    expect(ErrorCode::type, [&] { vector({1, 2}, 3); });
    expect(ErrorCode::type, [&] { vector({std::numeric_limits<float>::infinity()}); });
    expect(ErrorCode::type, [&] { vector({std::numeric_limits<float>::quiet_NaN()}); });
    expect(ErrorCode::type, [&] { tx.insert("docs", {std::string("bad"), vector({1}), Opaque(timestamps::type(), Bytes(7))}); });
    expect(ErrorCode::format, [&] { tx.create_table("badparams", {{"v", {vectors::type().id, 1, Bytes(3)}}}); });
    expect(ErrorCode::type, [&] { tx.create_table("badversion", {{"v", {vectors::type().id, 2, {}}}}); });
    tx.commit();

    auto distance = call("vector.squared_l2", {column("embedding"), literal(vector({1, 2}))});
    Query nearest{"docs", {column("name"), distance}, {}, {Order{distance}}, 2};
    auto result = db.query(nearest);
    CHECK(result.rows.size() == 2);
    CHECK(std::get<std::string>(result.rows[0][0]) == "near");
    CHECK(std::get<double>(result.rows[0][1]) == 0);
    CHECK(std::get<double>(result.rows[1][1]) == 2);
    nearest.where = Predicate{column("created"), Compare::greater, literal(timestamps::value(0))};
    CHECK(std::get<std::string>(db.query(nearest).rows[0][0]) == "middle");
    auto chronological = db.query(Query{"docs", {column("name")}, {}, {Order{column("created")}}});
    CHECK(std::get<std::string>(chronological.rows[0][0]) == "near");
    CHECK(std::get<std::string>(chronological.rows[2][0]) == "far");
    auto delta = call("timestamp.delta_us", {column("created"), literal(timestamps::value(10))});
    CHECK(std::get<std::int64_t>(db.query(Query{"docs", {delta}}).rows[0][0]) == 20);
    expect(ErrorCode::unsupported, [&] { db.query(Query{"empty", {}, {}, {Order{column("v")}}, 0}); });
    expect(ErrorCode::type, [&] { db.query(Query{"empty", {call("vector.squared_l2", {column("v")})}}); });
    expect(ErrorCode::type, [&] { db.query(Query{"empty", {call("vector.squared_l2", {column("v"), literal(1.0)})}}); });
    expect(ErrorCode::type, [&] { db.query(Query{"variable", {call("vector.squared_l2", {column("v"), literal(vector({1}))})}}); });
    expect(ErrorCode::type, [&] { db.query(Query{"fixed", {call("vector.squared_l2", {column("v"), literal(vector({1, 2}, 2))})}}); });
    auto overflow = call("timestamp.delta_us", {literal(timestamps::value(INT64_MAX)), literal(timestamps::value(-1))});
    expect(ErrorCode::type, [&] { db.query(Query{"docs", {overflow}}); });
    auto underflow = call("timestamp.delta_us", {literal(timestamps::value(INT64_MIN)), literal(timestamps::value(1))});
    expect(ErrorCode::type, [&] { db.query(Query{"docs", {underflow}}); });
    auto extremes = call("vector.squared_l2", {literal(vector({std::numeric_limits<float>::max()})), literal(vector({-std::numeric_limits<float>::max()}))});
    CHECK(std::isfinite(std::get<double>(db.query(Query{"docs", {extremes}, {}, {}, 1}).rows[0][0])));

    TempDirectory temp;
    db.save(temp.path / "addons.csql");
    auto reopened = Database::load(temp.path / "addons.csql", addons());
    CHECK(std::get<std::string>(reopened.query(nearest).rows[0][0]) == "middle");
    auto variable = reopened.query(Query{"variable"});
    CHECK(vectors::elements(variable.rows[0][0]).empty());
    CHECK(vectors::elements(variable.rows[1][0]) == std::vector<float>({1, 2, 3}));
    CHECK(vectors::elements(variable.rows[2][0]) == std::vector<float>({4}));
    CHECK(reopened.query(Query{"fixed"}).types[0] == vectors::type(3));
    auto timestamps = reopened.query(Query{"docs", {column("created")}});
    CHECK(timestamps::microseconds(timestamps.rows[1][0]) == -10);
    Registry vector_only; vectors::install(vector_only);
    expect(ErrorCode::type, [&] { Database::load(temp.path / "addons.csql", vector_only); });
}); }
