#include "check.hpp"
#include "../src/state.hpp"
#include <limits>
using namespace coresql;

int main() {
    return tests([] {
        Registry registry;
        std::vector<std::string> trace;
        registry.add(Function{"project", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> v) -> Value {
                                  auto n = std::get<std::int64_t>(v[0]);
                                  trace.push_back("project" + std::to_string(n));
                                  if (n == 3)
                                      throw Error(ErrorCode::type, "projection failure");
                                  return n == 1 ? std::numeric_limits<std::int64_t>::max() : 1;
                              }});
        registry.add(Function{"filter", [](std::span<const Type>) { return integer(); },
                              [&](std::span<const Value> v) -> Value {
                                  auto n = std::get<std::int64_t>(v[0]);
                                  trace.push_back("filter" + std::to_string(n));
                                  if (n == 4)
                                      throw Error(ErrorCode::state, "filter failure");
                                  return n;
                              }});
        Database db(registry);
        auto tx = db.begin();
        tx.create_table("t", {{"id", integer()}, {"g", text()}, {"v", integer(), false, {}, {}, true}});
        for (std::int64_t i = 1; i <= 600; ++i)
            tx.insert("t", {i, std::to_string(i % 3), i % 4 ? Value(i) : Value(Null(integer()))});
        tx.commit();
        Query q{"t",
                {column("g"), aggregate("count"), aggregate("sum", {column("v")}),
                 aggregate("avg", {column("v")}), aggregate("min", {column("v")}),
                 aggregate("max", {column("v")})}};
        q.group_by = {column("g")};
        q.order_by = {{column("g"), true}};
        q.select.push_back(aggregate("count", {column("v")}));
        q.select.back().distinct = true;
        auto expected = db.query(q);
        detail::retained_matches() = 0;
        q.repeatable = true;
        CHECK(db.query(q).rows == expected.rows);
        CHECK(detail::retained_matches() < 10); // No 600-row aggregate input retention.
        q.limit = 0;
        CHECK(db.query(q).rows.empty());
        q.limit = 1;
        CHECK(db.query(q).rows == std::vector<Row>{expected.rows.front()});
        q.where = Predicate{column("id"), Compare::less, literal(std::int64_t{0})};
        CHECK(db.query(q).rows.empty());
        Query empty{"t", {aggregate("count"), aggregate("sum", {column("v")})}, q.where};
        empty.repeatable = true;
        CHECK(db.query(empty).rows == std::vector<Row>({{std::int64_t{0}, Null(integer())}}));

        auto error = [&](Query query) {
            try {
                (void)db.query(query);
            } catch (const Error& e) {
                return std::pair{e.code, std::string(e.what())};
            }
            throw std::runtime_error("Expected error");
        };
        Query failing{"t",
                      {aggregate("sum", {call("project", {column("id")})})},
                      Predicate{column("id"), Compare::less_equal, literal(std::int64_t{3})}};
        trace.clear();
        auto original = error(failing);
        CHECK(trace == std::vector<std::string>({"project1", "project2", "project3"}));
        CHECK(original.second == "projection failure");
        failing.repeatable = true;
        CHECK(error(failing) == original); // Later projection wins over earlier SUM overflow.
        failing.where = Predicate{column("id"), Compare::less_equal, literal(std::int64_t{2})};
        auto streamed = error(failing);
        failing.repeatable = false;
        CHECK(error(failing) == streamed); // With no projection failure, report aggregate overflow.
        failing.where = Predicate{call("filter", {column("id")}), Compare::greater, literal(std::int64_t{0})};
        trace.clear();
        original = error(failing);
        CHECK(trace == std::vector<std::string>({"filter1", "filter2", "filter3", "filter4"}));
        CHECK(original.second == "filter failure");
        failing.repeatable = true;
        CHECK(error(failing) == original); // Later WHERE wins over projection and SUM failures.
        failing.limit = 0;
        trace.clear();
        CHECK(db.query(failing).rows.empty());
        CHECK(trace.empty());
    });
}
