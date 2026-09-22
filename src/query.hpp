#pragma once
#include "state.hpp"
#include "query_control.hpp"

namespace coresql::detail::execution {
// The caller retains both the table snapshot and registry until execution ends.
Result run(const Tables&, const Query&, const Registry&);
StreamResult stream(const Tables&, const Query&, const Registry&, const RowVisitor&);
QueryCursor make_cursor(Tables, Registry, Query, QueryOptions = {});
std::string materialize(Tables&, Result);
Result run_relations(const Tables&, const Query&, const Registry&);
} // namespace coresql::detail::execution
