#pragma once
#include "state.hpp"

namespace coresql::detail::execution {
// The caller retains both the table snapshot and registry until execution ends.
Result run(const Tables&, const Query&, const Registry&);
std::string materialize(Tables&, Result);
Result run_relations(const Tables&, const Query&, const Registry&);
} // namespace coresql::detail::execution
