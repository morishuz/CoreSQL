#pragma once
#include "core.hpp"

namespace coresql::vectors {
// Returns nullopt for variable-length vectors; validates type identity/parameters.
std::optional<std::uint64_t> dimensions(const Type&);
// A schema parameter, not a template argument. Empty means variable length.
Type type(std::optional<std::uint64_t> dimensions = {});
Value value(std::span<const float>, std::optional<std::uint64_t> dimensions = {});
std::vector<float> elements(const Value&);
void install(Registry&);
} // namespace coresql::vectors
