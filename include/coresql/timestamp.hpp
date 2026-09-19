#pragma once
#include "core.hpp"

namespace coresql::timestamps {
const Type& type();
Value value(std::int64_t unix_microseconds);
std::int64_t microseconds(const Value&);
void install(Registry&);
} // namespace coresql::timestamps
