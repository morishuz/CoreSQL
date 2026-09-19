#pragma once
#include "query.hpp"
#include "key_index.hpp"
#include "comparison.hpp"
#include <algorithm>
#include <array>

// Private execution machinery shared by reads and mutations. None of these
// bound values may outlive the registry and table snapshot used to bind them.
namespace coresql::detail::execution {
[[noreturn]] inline void fail(ErrorCode code, const std::string& message) {
    throw Error(code, message);
}

// Views retain references to input snapshots; joined rows are never concatenated.
struct RowView {
    const Row* left;
    const Row* right = nullptr;
    std::int64_t id = 0;
    RowView(const Row& row, std::int64_t rowid = 0) : left(&row), id(rowid) {}
    RowView(const Row& a, const Row& b, std::int64_t rowid = 0) : left(&a), right(&b), id(rowid) {}
    const Value& operator[](std::size_t i) const {
        return i < left->size() ? (*left)[i] : (*right)[i - left->size()];
    }
};
struct Scope {
    const detail::Table& left;
    const detail::Table* right = nullptr;
    std::string left_alias, right_alias;
    bool allow_identity = false;
    Scope(const detail::Table& table) : left(table) {}
    std::pair<std::size_t, Type> resolve(const Expr& expression) const {
        const auto* table = &left;
        std::size_t offset = 0;
        if (right && expression.qualifier.empty())
            fail(ErrorCode::schema, "Joined columns require an alias");
        if (!expression.qualifier.empty()) {
            if (right && expression.qualifier == right_alias) {
                table = right;
                offset = left.columns.size();
            } else if (expression.qualifier != left_alias)
                fail(ErrorCode::schema, "Unknown column alias: " + expression.qualifier);
        }
        auto found = std::find_if(table->columns.begin(), table->columns.end(),
                                  [&](const Column& c) { return c.name == expression.name; });
        if (found == table->columns.end())
            fail(ErrorCode::schema, "Unknown column: " + expression.name);
        return {offset + static_cast<std::size_t>(found - table->columns.begin()), found->type};
    }
};

// Borrow only from the query's bound literals or its retained table snapshot.
// Function operands own their temporaries through the comparison.
struct InspectedValue {
    const Value* borrowed = nullptr;
    Value owned = std::int64_t{0};
    const Value& get() const { return borrowed ? *borrowed : owned; }
};

struct BoundExpr {
    Type type;
    Expr::Kind kind;
    std::size_t index = 0;
    Value value = std::int64_t{0};
    const Function* function = nullptr;
    std::vector<BoundExpr> arguments;
    std::function<Value(std::span<const Value>)> subquery = {};
    std::function<Value(std::span<const Value>)> prepared = {};
    std::shared_ptr<std::optional<Value>> reused = {};

    template <class R> InspectedValue inspect(const R& row, const Registry& registry) const {
        if (kind == Expr::Kind::column)
            return {&row[index], std::int64_t{0}};
        if (kind == Expr::Kind::literal)
            return {&value, std::int64_t{0}};
        return {nullptr, evaluate(row, registry)};
    }
    template <class R> Value evaluate(const R& row, const Registry& registry) const {
        if (reused) {
            if (!*reused)
                *reused = evaluate_uncached(row, registry);
            return **reused;
        }
        return evaluate_uncached(row, registry);
    }
    template <class R> Value evaluate_uncached(const R& row, const Registry& registry) const {
        if (kind == Expr::Kind::row_id) {
            if constexpr (requires { row.id; })
                return row.id;
            else
                fail(ErrorCode::unsupported, "Row identity is only available in query expressions");
        }
        if (kind == Expr::Kind::column)
            return row[index];
        if (kind == Expr::Kind::literal)
            return value;
        if (kind == Expr::Kind::coalesce) {
            for (const auto& argument : arguments) {
                auto value = argument.evaluate(row, registry);
                if (!is_null(value))
                    return value;
            }
            return Null(type);
        }
        if (kind == Expr::Kind::conditional) {
            const std::size_t first = function ? 1 : 0;
            const bool fallback = (arguments.size() - first) % 2 != 0;
            const std::size_t end = arguments.size() - (fallback ? 1 : 0);
            Value base = std::int64_t{0};
            if (function)
                base = arguments[0].evaluate(row, registry);
            if (function && is_null(base))
                return fallback ? arguments.back().evaluate(row, registry) : Value(Null(type));
            for (std::size_t i = first; i < end; ++i) {
                auto condition = arguments[i++].evaluate(row, registry);
                if (function) {
                    if (is_null(base) || is_null(condition))
                        continue;
                    std::array<Value, 2> values{base, std::move(condition)};
                    condition = function->invoke(values);
                    if (type_of(condition) != integer() || !registry.addon(integer()).native_ops)
                        registry.validate(condition, integer());
                }
                if (!is_null(condition) && std::get<std::int64_t>(condition))
                    return arguments[i].evaluate(row, registry);
            }
            return fallback ? arguments.back().evaluate(row, registry) : Value(Null(type));
        }
        // Common unary/binary/ternary calls own their operands on the stack.
        // Larger calls retain dynamic storage; evaluation order is unchanged.
        std::array<Value, 3> local;
        std::vector<Value> dynamic;
        std::span<Value> values;
        if (arguments.size() <= local.size())
            values = std::span(local).first(arguments.size());
        else {
            dynamic.resize(arguments.size());
            values = dynamic;
        }
        for (std::size_t i = 0; i < arguments.size(); ++i)
            values[i] = arguments[i].evaluate(row, registry);
        if (subquery)
            return subquery(values);
        if (!function->accepts_null && std::any_of(values.begin(), values.end(), is_null))
            return Null(type);
        auto result = prepared ? prepared(values) : function->invoke(values);
        // Bound results of native_ops types are trusted when the cell identity matches.
        // Insert, recovery and public Registry::validate still check every value.
        if (is_null(result) || type_of(result) != type || !registry.addon(type).native_ops)
            registry.validate(result, type);
        return result;
    }
};

struct BoundLeaf {
    BoundExpr left, right;
    Compare operation;
    detail::Comparison comparison;
    bool values(const Value& a, const Value& b) const {
        if (is_null(a) || is_null(b))
            return false;
        if (operation == Compare::equal || operation == Compare::not_equal) {
            bool equal = comparison.equal(a, b);
            return operation == Compare::equal ? equal : !equal;
        }
        int c = comparison.compare(a, b);
        switch (operation) {
        case Compare::less:
            return c < 0;
        case Compare::greater:
            return c > 0;
        case Compare::less_equal:
            return c <= 0;
        case Compare::greater_equal:
            return c >= 0;
        default:
            fail(ErrorCode::schema, "Unknown predicate operation");
        }
    }
    template <class R> bool matches(const R& row, const Registry& registry) const {
        auto a = left.inspect(row, registry), b = right.inspect(row, registry);
        return values(a.get(), b.get());
    }
};
struct BoundMembership {
    BoundExpr left, right;
    detail::KeyBinding binding;
    template <class R> int truth(const R& row, const Registry& registry) const {
        auto source = left.inspect(row, registry), key = right.inspect(row, registry);
        if (is_null(source.get()) || is_null(key.get()))
            return -1;
        return binding.matches(source.get(), key.get());
    }
};
struct BoundPredicate {
    Predicate::Kind kind;
    std::optional<BoundLeaf> leaf;
    std::vector<BoundPredicate> children;
    std::unique_ptr<BoundMembership> membership;
    struct Memo {
        const Row* left = nullptr;
        int truth = 0;
    };
    std::unique_ptr<Memo> reused = {};
    // -1 is SQL UNKNOWN; WHERE accepts only TRUE.
    template <class R> int truth(const R& row, const Registry& registry) const {
        if constexpr (requires { row.left; }) {
            if (reused) {
                if (reused->left == row.left)
                    return reused->truth;
                int result = truth_uncached(row, registry);
                reused->left = row.left;
                reused->truth = result;
                return result;
            }
        }
        return truth_uncached(row, registry);
    }
    template <class R> int truth_uncached(const R& row, const Registry& registry) const {
        if (leaf) {
            auto a = leaf->left.inspect(row, registry), b = leaf->right.inspect(row, registry);
            if (is_null(a.get()) || is_null(b.get()))
                return -1;
            return leaf->values(a.get(), b.get());
        }
        switch (kind) {
        case Predicate::Kind::comparison:
            fail(ErrorCode::state, "Unbound comparison");
        case Predicate::Kind::contains:
            return membership->truth(row, registry);
        case Predicate::Kind::all: {
            int result = 1;
            for (const auto& child : children) {
                int t = child.truth(row, registry);
                if (!t)
                    return 0;
                if (t < 0)
                    result = -1;
            }
            return result;
        }
        case Predicate::Kind::any: {
            int result = 0;
            for (const auto& child : children) {
                int t = child.truth(row, registry);
                if (t == 1)
                    return 1;
                if (t < 0)
                    result = -1;
            }
            return result;
        }
        case Predicate::Kind::negation: {
            int t = children.front().truth(row, registry);
            return t < 0 ? -1 : !t;
        }
        }
        fail(ErrorCode::schema, "Unknown predicate kind");
    }
    template <class R> bool matches(const R& row, const Registry& registry) const {
        return truth(row, registry) == 1;
    }
    // Queries and updates share the fast column/literal comparison shape.
    const BoundLeaf* direct_comparison() const {
        return leaf && leaf->left.kind == Expr::Kind::column && leaf->right.kind == Expr::Kind::literal
                   ? &*leaf
                   : nullptr;
    }
    // Only a leading AND guard is extracted. Moving a later key test ahead of
    // a function could suppress an error on a row excluded by that key.
    BoundPredicate* leading_key_guard() {
        if (kind == Predicate::Kind::comparison || kind == Predicate::Kind::contains)
            return this;
        if (kind == Predicate::Kind::all && !children.empty())
            return children.front().leading_key_guard();
        return nullptr;
    }
};
BoundExpr bind(const Expr&, const Scope&, const Registry&, unsigned depth = 0);
std::optional<BoundPredicate> bind_predicate(const std::optional<Predicate>&, const Scope&, const Registry&);
std::optional<IndexResult> candidates(const Table&, std::optional<BoundPredicate>&);
void normalize(IndexResult&);

// Rewriting visits captured arguments but not a nested subquery's local scope.
void transform_expr(Expr&, const std::function<void(Expr&)>&);
void transform_pred(Predicate&, const std::function<void(Expr&)>&);
void transform_query(Query&, const std::function<void(Expr&)>&);
bool has_aggregate(const Expr&);
bool same_expression(const Expr&, const Expr&);
std::optional<Query> prepare_disjunctive_join(const Tables&, const Query&);
std::optional<Query> prepare_membership_join(const Tables&, const Query&, const Registry&);
extern thread_local const Tables* binding_tables;

} // namespace coresql::detail::execution
