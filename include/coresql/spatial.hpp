#pragma once
#include "coresql/core.hpp"

namespace coresql::spatial {
// Cartesian, axis-aligned boxes. No coordinate system or spherical geometry.
struct Box { double min_x, min_y, max_x, max_y; bool operator==(const Box&) const = default; };
Type type();
Value value(Box);
Box bounds(const Value&);
bool overlaps(Box, Box);
inline constexpr const char* index_id = "coresql.box.x_interval.v1";
void install(Registry&);
}
