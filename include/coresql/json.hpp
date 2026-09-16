#pragma once
#include "coresql/core.hpp"
#include <string_view>

namespace coresql::json {
Type type();
Value value(std::string_view document);
void install(Registry&);
// RFC 6901 pointer; no wildcards. Missing/null/nonmatching scalar => zero keys.
// Supported key types: text, integer (int64 JSON integer lexeme), real (finite double).
KeyExtractor property(std::string name, std::string_view pointer, Type key_type = text());
}
