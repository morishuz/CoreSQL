#include "membership_lookup.hpp"
#include "bound.hpp"
#include "index.hpp"
#include "correlated_source.hpp"

namespace coresql::detail::execution {
thread_local const Tables* binding_tables = nullptr;

// Conditional and coalescing expressions share exact result typing and literal
// NULL contextualization; branch evaluation remains lazy in BoundExpr.
Type result_type(std::vector<BoundExpr>& arguments, const std::vector<std::size_t>& arms) {
    std::optional<Type> type;
    for (auto i : arms) {
        const auto& arm = arguments[i];
        if (arm.kind == Expr::Kind::literal && is_null(arm.value))
            continue;
        if (type && *type != arm.type)
            fail(ErrorCode::type, "Expression result types differ");
        type = arm.type;
    }
    auto result = type.value_or(integer());
    for (auto i : arms) {
        auto& arm = arguments[i];
        if (arm.kind == Expr::Kind::literal && is_null(arm.value)) {
            arm.type = result;
            arm.value = Null(result);
        }
    }
    return result;
}

void transform_expr(Expr& e, const std::function<void(Expr&)>& f) {
    for (auto& a : e.arguments)
        transform_expr(a, f);
    f(e);
}
void transform_pred(Predicate& p, const std::function<void(Expr&)>& f) {
    transform_expr(p.left, f);
    transform_expr(p.right, f);
    for (auto& c : p.children)
        transform_pred(c, f);
}
void transform_query(Query& q, const std::function<void(Expr&)>& f) {
    for (auto& relation : q.relations) {
        if (!relation.query)
            fail(ErrorCode::schema, "Missing relation query");
        auto copy = *relation.query;
        transform_query(copy, f);
        relation.query = std::make_shared<const Query>(std::move(copy));
    }
    for (auto& [op, part] : q.compounds) {
        if (!part)
            fail(ErrorCode::schema, "Missing compound query");
        auto copy = *part;
        transform_query(copy, f);
        part = std::make_shared<const Query>(std::move(copy));
    }
    for (auto& e : q.select)
        transform_expr(e, f);
    for (auto& e : q.group_by)
        transform_expr(e, f);
    if (q.having)
        transform_pred(*q.having, f);
    if (q.where)
        transform_pred(*q.where, f);
    if (q.source_where)
        transform_pred(*q.source_where, f);
    for (auto& key : q.order_by)
        transform_expr(key.expression, f);
    auto transform_join = [&](Join& join) {
        transform_expr(join.left, f);
        transform_expr(join.right, f);
        if (join.right_where)
            transform_pred(*join.right_where, f);
        if (join.on)
            transform_pred(*join.on, f);
    };
    if (q.join)
        transform_join(*q.join);
    for (auto& join : q.joins)
        transform_join(join);
}
bool has_aggregate(const Expr& e) {
    return e.kind == Expr::Kind::aggregate ||
           std::any_of(e.arguments.begin(), e.arguments.end(), has_aggregate);
}
bool same_expression(const Expr& a, const Expr& b) {
    if (a.distinct != b.distinct || a.kind != b.kind || a.name != b.name || a.qualifier != b.qualifier ||
        a.value != b.value || a.subquery || b.subquery || a.arguments.size() != b.arguments.size())
        return false;
    for (std::size_t i = 0; i < a.arguments.size(); ++i)
        if (!same_expression(a.arguments[i], b.arguments[i]))
            return false;
    return true;
}
namespace {
// IN reserves its first argument for the needle; scalar/EXISTS queries contain
// captures only. Keep their scope-preserving substitution in one place.
auto substitute_parameters(const Expr& expression, std::size_t offset = 0) {
    return
        [query = expression.subquery, names = expression.parameters, offset](std::span<const Value> values) {
            auto bound = *query;
            transform_query(bound, [&](Expr& e) {
                if (e.kind != Expr::Kind::parameter)
                    return;
                auto found = std::find(names.begin(), names.end(), e.name);
                if (found == names.end())
                    fail(ErrorCode::schema, "Unbound query parameter");
                e = literal(values[offset + static_cast<std::size_t>(found - names.begin())]);
            });
            return bound;
        };
}
} // namespace

BoundExpr bind(const Expr& expression, const Scope& table, const Registry& registry, unsigned depth) {
    if (depth > 64)
        fail(ErrorCode::schema, "Expression nesting exceeds 64");
    BoundExpr result{integer(), expression.kind, 0, std::int64_t{0}, nullptr, nullptr, nullptr, {}};
    switch (expression.kind) {
    case Expr::Kind::column: {
        auto [index, type] = table.resolve(expression);
        result.index = index;
        result.type = std::move(type);
        break;
    }
    case Expr::Kind::row_id:
        if (!table.allow_identity || table.right)
            fail(ErrorCode::unsupported, "Row identity requires a single table");
        result.type = integer();
        break;
    case Expr::Kind::membership: {
        if (expression.arguments.empty())
            fail(ErrorCode::schema, "Membership needs a value");
        for (const auto& a : expression.arguments)
            result.arguments.push_back(bind(a, table, registry, depth + 1));
        const auto* equal = &registry.function(expression.name);
        bool same_type = true;
        auto validate = [&](Type candidate) {
            same_type = same_type && candidate == result.arguments[0].type;
            auto left = result.arguments[0].type;
            if (expression.arguments[0].kind == Expr::Kind::literal && is_null(expression.arguments[0].value))
                left = candidate;
            if (equal->infer(std::array<Type, 2>{left, candidate}) != integer())
                fail(ErrorCode::type, "Membership equality must return integer truth");
        };
        auto tables = binding_tables;
        auto substitute = substitute_parameters(expression, 1);
        if (expression.subquery) {
            if (!tables || expression.parameters.size() + 1 != expression.arguments.size())
                fail(ErrorCode::schema, "Invalid membership query");
            std::vector<Value> nulls;
            for (const auto& a : result.arguments)
                nulls.push_back(Null(a.type));
            auto probe = substitute(nulls);
            probe.limit = 0;
            auto shape = run(*tables, probe, registry);
            if (shape.types.size() != 1)
                fail(ErrorCode::schema, "Membership query must return one column");
            validate(shape.types[0]);
        } else
            for (std::size_t i = 1; i < result.arguments.size(); ++i) {
                auto& a = result.arguments[i];
                if (a.kind == Expr::Kind::literal && is_null(a.value)) {
                    a.type = result.arguments[0].type;
                    a.value = Null(a.type);
                }
                validate(a.type);
            }
        std::optional<std::vector<Value>> literals;
        if (!expression.subquery && equal->prepare_membership && same_type &&
            std::all_of(result.arguments.begin() + 1, result.arguments.end(),
                        [](const BoundExpr& a) { return a.kind == Expr::Kind::literal; })) {
            literals.emplace();
            for (std::size_t i = 1; i < result.arguments.size(); ++i)
                literals->push_back(result.arguments[i].value);
            result.arguments.resize(1);
        }
        const auto needle_type = result.arguments[0].type;
        const auto* truth = &registry.addon(integer());
        result.type = integer();
        result.subquery = [tables, &registry, equal, substitute, same_type, needle_type, truth,
                           literals = std::move(literals), lookup = MembershipLookup{},
                           query = bool(expression.subquery),
                           cacheable = expression.subquery && expression.subquery->repeatable &&
                                       expression.parameters.empty(),
                           cache = std::optional<Result>{}](std::span<const Value> values) mutable -> Value {
            bool unknown = false;
            auto matches = [&](const Value& candidate) {
                if (is_null(values[0]) || is_null(candidate)) {
                    unknown = true;
                    return false;
                }
                auto answer = equal->invoke(std::array<Value, 2>{values[0], candidate});
                detail::check_value(*truth, integer(), answer);
                if (is_null(answer)) {
                    unknown = true;
                    return false;
                }
                return std::get<std::int64_t>(answer) != 0;
            };
            if (query) {
#ifdef CORESQL_TESTING
                if (!(cacheable && cache))
                    if (auto* counters = detail::active_query_counters)
                        ++counters->subquery_executions;
#endif
                auto fresh = cacheable && cache ? Result{} : run(*tables, substitute(values), registry);
                if (cacheable && !cache)
                    cache = std::move(fresh);
                const auto& rows = (cacheable ? *cache : fresh).rows;
                if (cacheable && same_type) {
                    lookup.prepare(*equal, needle_type, rows,
                                   [](const Row& row) -> const Value& { return row[0]; });
                    if (auto answer = lookup.find(values[0]))
                        return *answer;
                }
                for (const auto& row : rows)
                    if (matches(row[0]))
                        return std::int64_t{1};
            } else if (literals) {
                lookup.prepare(*equal, needle_type, *literals,
                               [](const Value& value) -> const Value& { return value; });
                if (auto answer = lookup.find(values[0]))
                    return *answer;
                for (const auto& candidate : *literals)
                    if (matches(candidate))
                        return std::int64_t{1};
            } else
                for (std::size_t i = 1; i < values.size(); ++i)
                    if (matches(values[i]))
                        return std::int64_t{1};
            return unknown ? Value(Null(integer())) : Value(std::int64_t{0});
        };
        break;
    }
    case Expr::Kind::subquery:
    case Expr::Kind::exists: {
        if (!binding_tables || !expression.subquery ||
            expression.parameters.size() != expression.arguments.size())
            fail(ErrorCode::schema, "Invalid scalar subquery");
        auto tables = binding_tables;
        std::vector<Type> types;
        for (const auto& a : expression.arguments) {
            result.arguments.push_back(bind(a, table, registry, depth + 1));
            types.push_back(result.arguments.back().type);
        }
        auto substitute = substitute_parameters(expression);
        std::vector<Value> nulls;
        for (const auto& t : types)
            nulls.push_back(Null(t));
        auto probe = substitute(nulls);
        probe.limit = 0;
        auto shape = run(*tables, probe, registry);
        bool existence = expression.kind == Expr::Kind::exists;
        if (!existence && shape.types.size() != 1)
            fail(ErrorCode::schema, "Scalar subquery must return one column");
        result.type = existence ? integer() : shape.types[0];
        auto source = correlated_source(*tables, *expression.subquery, expression.parameters, registry);
        auto cache = expression.subquery->repeatable &&
                             std::all_of(types.begin(), types.end(),
                                         [&](const Type& t) { return native_i64(registry.addon(t)); })
                         ? std::make_shared<SubqueryCache>()
                         : nullptr;
        result.subquery = [tables, &registry, substitute, type = result.type, existence, source,
                           cache](std::span<const Value> values) mutable -> Value {
            auto evaluate = [&]() -> Value {
                auto q = substitute(values);
                q.limit = std::min(q.limit, std::size_t{1});
                if (existence) {
                    if (!q.limit)
                        return std::int64_t{0};
                    bool reduction = std::any_of(q.select.begin(), q.select.end(), has_aggregate);
                    if (reduction && q.group_by.empty() && !q.having && q.compounds.empty())
                        return std::int64_t{1};
                    if (!reduction && q.group_by.empty() && !q.having && q.compounds.empty()) {
                        q.select = {literal(std::int64_t{1})};
                        q.order_by.clear();
                        q.distinct = false;
                    }
                }
#ifdef CORESQL_TESTING
                if (auto* counters = detail::active_query_counters)
                    ++counters->subquery_executions;
#endif
                auto execute = [&]() {
                    if (source)
                        if (auto subset = source->subset(values)) {
                            auto working = *tables;
                            std::string name = "\x01"
                                               "correlated";
                            while (working.contains(name))
                                name += '_';
                            working.emplace(name, std::move(subset));
                            q.table = name;
                            return run(working, q, registry);
                        }
                    return run(*tables, q, registry);
                };
                auto rows = execute();
                if (existence)
                    return std::int64_t(!rows.rows.empty());
                return rows.rows.empty() ? Value(Null(type)) : rows.rows[0][0];
            };
            return cache ? cache->get(values, evaluate) : evaluate();
        };
        break;
    }
    case Expr::Kind::conditional: {
        const std::size_t first = expression.name.empty() ? 0 : 1;
        if (expression.arguments.size() < first + 2)
            fail(ErrorCode::schema, "Conditional needs a branch");
        const bool fallback = (expression.arguments.size() - first) % 2 != 0;
        const std::size_t end = expression.arguments.size() - (fallback ? 1 : 0);
        if (first) {
            result.function = &registry.function(expression.name);
            result.truth_addon = &registry.addon(integer());
        }
        for (const auto& a : expression.arguments)
            result.arguments.push_back(bind(a, table, registry, depth + 1));
        auto null_literal = [](const BoundExpr& e) {
            return e.kind == Expr::Kind::literal && is_null(e.value);
        };
        for (std::size_t i = first; i < end; i += 2) {
            if (first) {
                std::array<Type, 2> types{result.arguments[0].type, result.arguments[i].type};
                if (null_literal(result.arguments[0]))
                    types[0] = types[1];
                if (null_literal(result.arguments[i]))
                    types[1] = types[0];
                if (result.function->infer(types) != integer())
                    fail(ErrorCode::type, "Conditional equality must return integer truth");
            } else if (result.arguments[i].type != integer())
                fail(ErrorCode::type, "Conditional needs integer truth");
        }
        std::vector<std::size_t> arms;
        for (std::size_t i = first + 1; i < end; i += 2)
            arms.push_back(i);
        if (fallback)
            arms.push_back(result.arguments.size() - 1);
        result.type = result_type(result.arguments, arms);
        break;
    }
    case Expr::Kind::coalesce: {
        if (expression.arguments.empty())
            fail(ErrorCode::schema, "Coalesce needs operands");
        std::vector<std::size_t> arms;
        for (const auto& a : expression.arguments) {
            arms.push_back(result.arguments.size());
            result.arguments.push_back(bind(a, table, registry, depth + 1));
        }
        result.type = result_type(result.arguments, arms);
        break;
    }
    case Expr::Kind::literal:
        result.type = type_of(expression.value);
        registry.validate(expression.value, result.type);
        result.value = expression.value;
        break;
    case Expr::Kind::call: {
        result.function = &registry.function(expression.name);
        std::vector<Type> types;
        for (const auto& argument : expression.arguments) {
            result.arguments.push_back(bind(argument, table, registry, depth + 1));
            types.push_back(result.arguments.back().type);
        }
        result.type = result.function->infer(types);
        registry.validate(result.type);
        result.result_addon = &registry.addon(result.type);
        if (result.function->prepare) {
            result.prepared = result.function->prepare(types, result.type);
            if (!result.prepared)
                fail(ErrorCode::type, "Function preparation returned no callback");
        }
        break;
    }
    default:
        fail(ErrorCode::schema, "Unknown expression kind");
    }
    return result;
}

BoundPredicate bind_condition(const Predicate& predicate, const Scope& table, const Registry& registry,
                              unsigned depth, std::size_t& nodes) {
    if (depth > 64 || ++nodes > 4096)
        fail(ErrorCode::schema, "Predicate exceeds depth 64 or 4096 nodes");
    BoundPredicate result{predicate.kind, {}, {}, {}};
    if (predicate.kind == Predicate::Kind::contains) {
        if (!predicate.children.empty())
            fail(ErrorCode::schema, "Membership cannot have child predicates");
        auto left = bind(predicate.left, table, registry, depth);
        auto right = bind(predicate.right, table, registry, depth);
        const auto& extractor = registry.extractor(predicate.extractor);
        if (left.type != extractor.source_type || right.type != extractor.key_type)
            fail(ErrorCode::type, "Membership operand types differ from extractor");
        result.membership = std::make_unique<BoundMembership>(BoundMembership{
            std::move(left), std::move(right), {extractor, registry.addon(extractor.key_type)}});
    } else if (predicate.kind == Predicate::Kind::comparison) {
        if (!predicate.children.empty())
            fail(ErrorCode::schema, "Comparison cannot have child predicates");
        auto left = bind(predicate.left, table, registry, depth);
        auto right = bind(predicate.right, table, registry, depth);
        if (left.type != right.type)
            fail(ErrorCode::type, "Predicate operand types differ");
        switch (predicate.operation) {
        case Compare::equal:
        case Compare::not_equal:
        case Compare::less:
        case Compare::greater:
        case Compare::less_equal:
        case Compare::greater_equal:
            break;
        default:
            fail(ErrorCode::schema, "Unknown predicate operation");
        }
        detail::Comparison comparison(registry, left.type,
                                      predicate.operation == Compare::equal ||
                                          predicate.operation == Compare::not_equal);
        result.leaf.emplace(BoundLeaf{std::move(left), std::move(right), predicate.operation, comparison});
    } else {
        switch (predicate.kind) {
        case Predicate::Kind::all:
        case Predicate::Kind::any:
            break;
        case Predicate::Kind::negation:
            if (predicate.children.size() != 1)
                fail(ErrorCode::schema, "NOT requires one child");
            break;
        default:
            fail(ErrorCode::schema, "Unknown predicate kind");
        }
        // Bind every branch, regardless of eventual short-circuit evaluation.
        for (const auto& child : predicate.children)
            result.children.push_back(bind_condition(child, table, registry, depth + 1, nodes));
    }
    return result;
}
std::optional<BoundPredicate> bind_predicate(const std::optional<Predicate>& predicate, const Scope& table,
                                             const Registry& registry) {
    if (!predicate)
        return {};
    std::size_t nodes = 0;
    return bind_condition(*predicate, table, registry, 0, nodes);
}

// A proof applies only to the predicate that selected these snapshot-local rows.
// Never move a later condition ahead of earlier potentially throwing functions.
std::optional<IndexResult> candidates(const detail::Table& table, std::optional<BoundPredicate>& predicate,
                                      const Registry& registry) {
    auto* guard = predicate ? predicate->leading_key_guard() : nullptr;
    std::optional<IndexResult> result;
    if (guard && guard->membership) {
        const auto& member = *guard->membership;
        if (member.left.kind == Expr::Kind::column && member.right.kind == Expr::Kind::literal &&
            !is_null(member.right.value))
            for (const auto& index : table.indexes)
                if (index->column == member.left.index &&
                    table.columns[index->column].index == member.binding.extractor.name) {
                    result = index->data->search("equal", member.right.value);
                    break;
                }
    }
    if (table.primary && guard && guard->leaf && guard->leaf->operation == Compare::equal) {
        const auto* left = &guard->leaf->left;
        const auto* right = &guard->leaf->right;
        if (left->kind == Expr::Kind::literal)
            std::swap(left, right);
        if (left->kind == Expr::Kind::column && left->index == table.primary->column &&
            right->kind == Expr::Kind::literal) {
            result = IndexResult{{}, true};
            if (!is_null(right->value))
                if (auto location = detail::lookup(table, right->value))
                    result->rows.push_back(*location);
        }
    }
    if (!result)
        result = composite_candidates(table, predicate, registry);
    if (!result && guard && guard->leaf) {
        const auto& leaf = *guard->leaf;
        if (leaf.left.kind == Expr::Kind::column && leaf.right.kind == Expr::Kind::literal &&
            !is_null(leaf.right.value)) {
            for (const auto& index : table.ordered)
                if (index->columns.front() == leaf.left.index) {
                    if (leaf.operation == Compare::equal) {
                        result = IndexResult{index->range(leaf.right.value, leaf.right.value), false};
                        break;
                    }
                    if (predicate->kind == Predicate::Kind::all && predicate->children.size() >= 2) {
                        const auto& next = predicate->children[1];
                        if (next.leaf && next.leaf->left.kind == Expr::Kind::column &&
                            next.leaf->left.index == leaf.left.index &&
                            next.leaf->right.kind == Expr::Kind::literal &&
                            !is_null(next.leaf->right.value) && leaf.operation == Compare::greater_equal &&
                            next.leaf->operation == Compare::less_equal) {
                            result =
                                IndexResult{index->range(leaf.right.value, next.leaf->right.value), false};
                            break;
                        }
                    }
                }
        }
    }
    if (result && result->exact) {
        if (guard == &*predicate)
            predicate.reset();
        else {
            guard->kind = Predicate::Kind::all;
            guard->leaf.reset();
            guard->membership.reset();
        }
    }
    return result;
}
void normalize(IndexResult& result) {
    std::sort(result.rows.begin(), result.rows.end());
    result.rows.erase(std::unique(result.rows.begin(), result.rows.end()), result.rows.end());
}
} // namespace coresql::detail::execution

namespace coresql {
Value evaluate_constant(const Expr& expression, const Registry& registry) {
    using namespace detail::execution;
    auto check = expression;
    transform_expr(check, [](Expr& e) {
        if (e.kind != Expr::Kind::literal && e.kind != Expr::Kind::call &&
            e.kind != Expr::Kind::conditional && e.kind != Expr::Kind::coalesce)
            fail(ErrorCode::unsupported, "Expected constant expression");
    });
    detail::Table empty;
    auto bound = bind(expression, Scope(empty), registry);
    return bound.evaluate(Row{}, registry);
}
} // namespace coresql
