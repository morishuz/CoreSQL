#pragma once
// Private, single-thread diagnostic bridge only; never linked into the library.
namespace allocation_profile {
void begin();
void end();
} // namespace allocation_profile
