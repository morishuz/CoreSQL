#pragma once
#include "coresql/core.hpp"
#include <string_view>

namespace coresql::sql {
// An adapter supplies SQL policy, not grammar or transaction behavior. Callbacks
// must be deterministic and side-effect-free; captured objects must be owned.
struct SqlOperation {
    std::string function;
    // Optional exact interpretation of a numeric literal in this operation.
    std::function<Value(std::string_view)> numeric_literal = {};
    std::optional<Type> null_type = {};
    bool repeatable = false;
};
enum class SqlAffinity { none, numeric, text };
struct SqlDeclaration {
    std::string_view name;
    std::span<const std::uint64_t> parameters;
    bool explicit_cast = false;
};
struct SqlTypeAdapter {
    std::string type_id;
    std::uint32_t version = 1;
    std::vector<std::string> names;
    std::function<Type(const SqlDeclaration&)> declaration;
    std::function<void(Registry&)> install;
    std::function<bool(const Type&, const Type&)> can_convert;
    std::function<Value(const Value&, const Type&, bool explicit_cast)> convert;
    // An empty type ID in function arguments denotes an untyped NULL literal.
    std::function<std::optional<SqlOperation>(std::string_view, std::span<const Type>)> operation = {};
    std::function<std::optional<Type>(std::span<const Type>)> common_type = {};
    std::function<Value(std::string_view)> typed_literal = {};
    // Assignment/CAST interprets numeric spelling as text before conversion.
    bool exact_numeric_input = false;
    // Successful literal casts may be folded; failed casts stay lazy.
    bool fold_text_cast = false;
    // Explicit promise allowing SQL's bounded subquery reuse for this type.
    bool repeatable = false;
    // Same-type comparisons cannot throw on valid values; enables safe pruning.
    bool reorder_comparisons = false;
    // SQL comparison precedence is shared; the adapter supplies its affinity policy.
    SqlAffinity affinity = SqlAffinity::none;
    // Preserve already-affiliated values: the native type itself, and both numeric
    // types for numeric affinity. Collation/custom equality belongs to TypeAddon.
    std::function<Value(const Value&)> apply_affinity = {};
};
class TypeAdapters {
public:
    explicit TypeAdapters(bool scalar_defaults = true); // false starts completely empty.
    void add(SqlTypeAdapter);
    // Returned pointers remain valid until this configuration is mutated/destroyed.
    const SqlTypeAdapter* named(std::string_view) const;
    const SqlTypeAdapter* find(const Type&) const;
    void install(Registry&) const;
    bool convertible(const Type&, const Type&) const;
    Value convert(const Value&, const Type&, bool explicit_cast) const;
    // No match returns nullopt; ambiguous claims and conversion failures still throw.
    std::optional<Value> try_convert(const Value&, const Type&, bool explicit_cast) const;
    std::optional<SqlOperation> operation(std::string_view, std::span<const Type>) const;
    std::optional<Type> common_type(std::span<const Type>) const;
    bool same_configuration(const TypeAdapters&) const;

private:
    std::shared_ptr<const std::vector<SqlTypeAdapter>> adapters_;
    const SqlTypeAdapter* conversion(const Type&, const Type&) const;
};
const TypeAdapters& default_type_adapters();
SqlTypeAdapter integer_adapter();
SqlTypeAdapter real_adapter();
SqlTypeAdapter text_adapter();
SqlTypeAdapter decimal_adapter();
SqlTypeAdapter date_adapter();
SqlTypeAdapter vector_adapter();
SqlTypeAdapter blob_adapter();
} // namespace coresql::sql
