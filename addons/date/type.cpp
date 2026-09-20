#include "coresql/date.hpp"
#include <chrono>
#include <cstring>

namespace coresql::dates {
namespace {
using namespace std::chrono;
constexpr auto id = "coresql.date.days";
constexpr auto first = sys_days(year{1} / January / 1).time_since_epoch().count();
constexpr auto last = sys_days(year{9999} / December / 31).time_since_epoch().count();
void check(std::int64_t count) {
    if (count < first || count > last)
        throw Error(ErrorCode::type, "DATE is outside 0001-01-01..9999-12-31");
}
} // namespace
const Type& type() {
    static const Type identity{id, 1, {}};
    return identity;
}
Value value(std::int64_t unix_days) {
    check(unix_days);
    return compact(intern(type()), unix_days);
}
std::int64_t days(const Value& input) {
    auto* cell = std::get_if<Compact>(&input);
    if (!cell || cell->bytes().size() != 8 || cell->type().id != id || cell->type().version != 1)
        throw Error(ErrorCode::type, "Expected DATE value");
    std::int64_t count = 0;
    std::memcpy(&count, cell->bytes().data(), 8);
    check(count);
    return count;
}
Value parse(std::string_view input) {
    if (input.size() != 10 || input[4] != '-' || input[7] != '-')
        throw Error(ErrorCode::type, "DATE requires YYYY-MM-DD");
    auto number = [&](std::size_t start, std::size_t length) {
        unsigned result = 0;
        for (auto c : input.substr(start, length)) {
            if (c < '0' || c > '9')
                throw Error(ErrorCode::type, "DATE requires YYYY-MM-DD");
            result = result * 10 + static_cast<unsigned>(c - '0');
        }
        return result;
    };
    auto y = number(0, 4), m = number(5, 2), d = number(8, 2);
    auto calendar = year{static_cast<int>(y)} / month{m} / day{d};
    if (y == 0 || !calendar.ok())
        throw Error(ErrorCode::type, "Invalid calendar DATE");
    return value(sys_days(calendar).time_since_epoch().count());
}
std::string format(const Value& input) {
    auto calendar = year_month_day(sys_days(std::chrono::days{dates::days(input)}));
    auto y = static_cast<unsigned>(static_cast<int>(calendar.year()));
    auto m = static_cast<unsigned>(calendar.month()), d = static_cast<unsigned>(calendar.day());
    std::string result = "0000-00-00";
    auto put = [&](unsigned n, std::size_t end, unsigned length) {
        while (length--) {
            result[--end] = static_cast<char>('0' + n % 10);
            n /= 10;
        }
    };
    put(y, 4, 4);
    put(m, 7, 2);
    put(d, 10, 2);
    return result;
}
void install(Registry& registry) {
    registry.add(Function{
        "date.year",
        [](std::span<const Type> t) {
            if (t.size() != 1 || t[0] != type())
                throw Error(ErrorCode::type, "YEAR expects DATE");
            return integer();
        },
        [](std::span<const Value> v) -> Value {
            return std::int64_t(
                int(std::chrono::year_month_day(std::chrono::sys_days(std::chrono::days{dates::days(v[0])}))
                        .year()));
        }});
    registry.add(
        TypeAddon{id, 1, Layout::i64,
                  [](ByteView p) {
                      if (!p.empty())
                          throw Error(ErrorCode::type, "DATE has no type parameters");
                  },
                  [](ByteView, const Value& v) { (void)dates::days(v); },
                  [](ByteView, const Value& a, const Value& b) { return dates::days(a) == dates::days(b); },
                  [](ByteView, const Value& a, const Value& b) {
                      auto x = dates::days(a), y = dates::days(b);
                      return (x > y) - (x < y);
                  },
                  [](ByteView, const Value& v) { return std::hash<std::int64_t>{}(dates::days(v)); }, true});
    registry.add(Function{"date.parse",
                          [](std::span<const Type> t) {
                              if (t.size() != 1 || t[0] != text())
                                  throw Error(ErrorCode::type, "date.parse expects TEXT");
                              return type();
                          },
                          [](std::span<const Value> v) { return parse(std::get<std::string>(v[0])); }});
    registry.add(Function{"date.format",
                          [](std::span<const Type> t) {
                              if (t.size() != 1 || t[0] != type())
                                  throw Error(ErrorCode::type, "date.format expects DATE");
                              return text();
                          },
                          [](std::span<const Value> v) -> Value { return format(v[0]); }});
}
} // namespace coresql::dates
