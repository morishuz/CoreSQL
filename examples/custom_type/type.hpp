#pragma once
#include "coresql/core.hpp"
#include <string_view>

namespace example::codes {
// Exactly two uppercase ASCII letters; this is not a country-code database.
coresql::Type type();
coresql::Value value(std::string_view);
std::string string(const coresql::Value&);
void install(coresql::Registry&);
} // namespace example::codes
