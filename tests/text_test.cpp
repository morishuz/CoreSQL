#include "check.hpp"
#include "coresql/sql.hpp"
#include <array>
using namespace coresql;
int main() {
    return tests([] {
        Registry registry;
        sql::install(registry);
        Database db(registry);
        sql::Connection sql(db, registry);
        auto length = [&](Value value) {
            std::array<Value, 1> args{std::move(value)};
            return sql.execute("SELECT length(?)", args).rows.at(0).at(0);
        };
        auto substring = [&](Value value) {
            std::array<Value, 1> args{std::move(value)};
            return sql.execute("SELECT SUBSTRING(? FROM 1)", args).rows.at(0).at(0);
        };
        CHECK(length(std::string{}) == Value(std::int64_t{0}));
        CHECK(length(std::string("é🙂")) == Value(std::int64_t{2}));
        for (auto s : {std::string("a\0b", 3), std::string("a\0", 2)}) {
            CHECK(length(s) == Value(std::int64_t{1}));
            CHECK(substring(s) == Value(s));
        }
        CHECK(length(std::string("\0a", 2)) == Value(std::int64_t{0}));
        // LENGTH retains its permissive byte-counting behavior; SUBSTRING is strict UTF-8.
        for (auto s : {std::string("\xc0\xaf", 2), std::string("\xf0\x9f", 2)}) {
            CHECK(length(s) == Value(std::int64_t{1}));
            expect(ErrorCode::constraint, [&] { substring(s); });
        }
        CHECK(length(std::string("\x80", 1)) == Value(std::int64_t{0}));
        expect(ErrorCode::constraint, [&] { substring(std::string("\x80", 1)); });
        CHECK(is_null(length(Null(text()))));
        CHECK(is_null(substring(Null(text()))));
    });
}
