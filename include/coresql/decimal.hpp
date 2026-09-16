#pragma once
#include "core.hpp"

namespace coresql::decimals {
struct Format {
    unsigned precision, scale;
};
Type type(unsigned precision = 18, unsigned scale = 3);
bool is_decimal(const Type&);
Format format_of(const Type&);
// Exact input by default; explicit rounding is half away from zero.
Value parse(std::string_view, const Type&, bool round = false);
Value literal(std::string_view);
Value convert(const Value&, const Type&, bool round = false);
std::string format(const Value&);
double to_real(const Value&);
std::int64_t to_integer(const Value&, bool truncate = false);
void install(Registry&);
} // namespace coresql::decimals
