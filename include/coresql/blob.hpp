#pragma once
#include "core.hpp"
namespace coresql::blobs {
const Type& type();
Value value(ByteView);
ByteView bytes(const Value&);
Value from_hex(std::string_view);
std::string hex(const Value&);
void install(Registry&);
} // namespace coresql::blobs
