#pragma once
#include "query.hpp"

namespace coresql::detail::execution {
// Internal stages. Enter through run() so binding context, nesting limits and
// stage precedence are established. Recursive queries must also use run().
Result run_filtered_join(const Tables&, const Query&, const Registry&);
Result run_general_join(const Tables&, const Query&, const Registry&);
Result run_multi_join(const Tables&, const Query&, const Registry&);
Result run_grouped(const Tables&, const Query&, const Registry&);
Result run_compound(const Tables&, const Query&, const Registry&);
Result run_distinct(const Tables&, const Query&, const Registry&);

// The consumer borrows each projected row only during the call. It is used for
// repeatable aggregate input without ordering/distinct/grouping; it must not
// retain the span. Early predicates must preserve the original error ordering.
using RowConsumer = std::function<void(std::span<const Value>)>;
Result run_scan(const Tables&, const Query&, const Registry&, const std::optional<Predicate>& early = {},
                const RowConsumer* consumer = nullptr);
} // namespace coresql::detail::execution
