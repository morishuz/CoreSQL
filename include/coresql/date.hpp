#pragma once
#include "core.hpp"

namespace coresql::dates {
// Proleptic Gregorian calendar dates, 0001-01-01 through 9999-12-31.
// Compact i64 cell: signed days since 1970-01-01. Persistence is eight LE bytes.
const Type& type();
Value parse(std::string_view iso_date);
Value value(std::int64_t unix_days);
std::int64_t days(const Value&);
std::string format(const Value&);
void install(Registry&);
} // namespace coresql::dates
