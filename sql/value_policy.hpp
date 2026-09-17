#pragma once
#include "coresql/sql_types.hpp"

namespace coresql::sql::detail {
Type any_type();
bool scalar_type(const Type&);
bool convertible(const Type& source, const Type& target, const TypeAdapters& = default_type_adapters());
Value convert(Value, const Type&, bool explicit_cast = false, const TypeAdapters& = default_type_adapters());
Value affinity(Value, const Type&, const TypeAdapters& = default_type_adapters());
// Shared SQL scalar expression policy, independent of individual type adapters.
void install_scalar_policy(Registry&);
std::optional<SqlOperation> scalar_operation(std::string_view, std::span<const Type>);
std::optional<Type> scalar_common_type(std::span<const Type>);
std::string affinity_function(const Type&, bool equality = false);
void install_coercions(Registry&, const TypeAdapters&);
void install_string_functions(Registry&);
void install_type_conversions(Registry&, const TypeAdapters&);
Value pack(const Value&);
Value unpack(const Value&);
} // namespace coresql::sql::detail
