#include "lower.hpp"
#include <set>

namespace coresql::sql::detail {
namespace {
void conjuncts(const Node& n, std::vector<Node>& out) {
    if (n.kind == Node::binary && n.name == "and") {
        for (const auto& a : n.args)
            conjuncts(a, out);
    } else
        out.push_back(n);
}
std::set<std::string> qualifiers(const Node& n) {
    std::set<std::string> out;
    if (n.kind == Node::column)
        out.insert(n.qualifier);
    for (const auto& a : n.args) {
        auto q = qualifiers(a);
        out.insert(q.begin(), q.end());
    }
    return out;
}
// A necessary local guard for a predicate already proven safe to rearrange.
// Every OR arm must contribute; keep the original predicate to test correlations.
std::optional<Node> local_guard(const Node& n, const std::string& alias) {
    if (qualifiers(n) == std::set<std::string>{alias})
        return n;
    if (n.kind != Node::binary || (n.name != "and" && n.name != "or"))
        return {};
    Node guard;
    guard.kind = Node::binary;
    guard.name = n.name;
    for (const auto& child : n.args) {
        auto part = local_guard(child, alias);
        if (part)
            guard.args.push_back(std::move(*part));
        else if (n.name == "or")
            return {};
    }
    if (guard.args.empty())
        return {};
    if (guard.args.size() == 1)
        return std::move(guard.args[0]);
    return guard;
}
bool safe_prefix(const Node& n, const Lowerer& lower) {
    if (n.kind == Node::binary && (n.name == "and" || n.name == "or"))
        return std::all_of(n.args.begin(), n.args.end(),
                           [&](const Node& a) { return safe_prefix(a, lower); });
    if (n.kind == Node::unary && n.name == "not")
        return safe_prefix(n.args[0], lower);
    bool comparison_node =
        n.kind == Node::binary && (comparison(n.name) || n.name == "between" || n.name == "like");
    bool constant_membership = n.kind == Node::membership && !n.query;
    if (!comparison_node && !constant_membership)
        return false;
    for (const auto& a : n.args)
        if (a.kind != Node::column && a.kind != Node::literal && a.kind != Node::parameter)
            return false;
    auto type = lower.expression_type(lower.expression(n.args[0]));
    return type && (n.name != "like" || *type == text()) &&
           (*type == integer() || *type == real() || *type == text() ||
            (lower.types->find(*type) && lower.types->find(*type)->reorder_comparisons)) &&
           std::all_of(n.args.begin() + 1, n.args.end(),
                       [&](const Node& a) { return lower.expression_type(lower.expression(a)) == type; });
}

void plan_from(const Select& s, const Lowerer& local, Query& q) {
    const auto& schema = local.schema;
    std::set<std::string> aliases;
    for (const auto& source : s.sources) {
        table(schema, source.table);
        if (!aliases.insert(source.alias).second)
            throw Error(ErrorCode::schema, "Duplicate table alias");
    }
    if (!s.sources.empty())
        q.table = s.sources[0].table;
    q.distinct = s.distinct;
    if (s.sources.size() <= 1) {
        if (s.where)
            q.where = local.predicate(*s.where);
    } else if (std::any_of(s.sources.begin(), s.sources.end(),
                           [](const Source& source) { return source.explicit_join; })) {
        q.alias = s.sources[0].alias;
        for (std::size_t i = 1; i < s.sources.size(); ++i) {
            const auto& source = s.sources[i];
            Join join;
            join.table = source.table;
            join.alias = source.alias;
            join.cross = true;
            join.kind = source.kind;
            if (source.on)
                join.on = local.predicate(*source.on);
            if (join.kind == JoinKind::inner && join.on && join.on->kind == Predicate::Kind::comparison &&
                join.on->operation == Compare::equal && join.on->left.kind == Expr::Kind::column &&
                join.on->right.kind == Expr::Kind::column &&
                join.on->left.qualifier != join.on->right.qualifier &&
                (join.on->left.qualifier == source.alias || join.on->right.qualifier == source.alias)) {
                join.left = join.on->left;
                join.right = join.on->right;
                join.cross = false;
                join.on.reset();
            }
            q.joins.push_back(std::move(join));
        }
        if (s.where)
            q.where = local.predicate(*s.where);
    } else {
        // A bounded equality-join planner: only pure column/literal conjuncts may
        // be rearranged. Never reorder a potentially failing user function.
        std::vector<Node> conditions;
        if (s.where)
            conjuncts(*s.where, conditions);
        bool reorder = std::all_of(conditions.begin(), conditions.end(),
                                   [&](const Node& n) { return safe_prefix(n, local); });
        if (!reorder) {
            q.alias = s.sources[0].alias;
            // Preserve FROM order and callback order. Only leading integer key
            // equalities may eliminate tuples before the first unsafe conjunct.
            // Keep the complete WHERE predicate, including later join conditions.
            auto end = std::find_if(conditions.begin(), conditions.end(),
                                    [&](const Node& n) { return !safe_prefix(n, local); });
            std::set<std::string> available{s.sources[0].alias};
            for (std::size_t i = 1; i < s.sources.size(); ++i) {
                const auto& source = s.sources[i];
                Join join{source.table, source.alias, literal(std::int64_t{0}), literal(std::int64_t{0}),
                          true};
                for (auto it = conditions.begin(); it != end; ++it) {
                    const auto& c = *it;
                    if (c.kind != Node::binary || (c.name != "=" && c.name != "==") ||
                        c.args[0].kind != Node::column || c.args[1].kind != Node::column)
                        continue;
                    auto a = local.expression(c.args[0]), b = local.expression(c.args[1]);
                    if (local.expression_type(a) != integer() || local.expression_type(b) != integer())
                        continue;
                    if ((a.qualifier == source.alias && available.contains(b.qualifier)) ||
                        (b.qualifier == source.alias && available.contains(a.qualifier))) {
                        join.left = std::move(a);
                        join.right = std::move(b);
                        join.cross = false;
                        break;
                    }
                }
                q.joins.push_back(std::move(join));
                available.insert(source.alias);
            }
            if (s.where)
                q.where = local.predicate(*s.where);
        } else {
            // Resolve unqualified names once through the same column resolver used
            // by projections; planning then operates on unambiguous source aliases.
            std::function<void(Node&)> qualify = [&](Node& n) {
                if (n.kind == Node::column) {
                    auto e = local.expression(n);
                    if (e.kind == Expr::Kind::parameter) {
                        // Outer references are fixed inputs for this execution, not
                        // local join edges. Retain their capture and type/affinity
                        // while allowing local predicates to keep indexed paths.
                        n.kind = Node::captured_column;
                        n.name = e.name;
                        n.qualifier.clear();
                    } else {
                        if (e.kind != Expr::Kind::column)
                            unsupported("Joined row identity is unsupported");
                        n.name = e.name;
                        n.qualifier = e.qualifier;
                    }
                }
                for (auto& a : n.args)
                    qualify(a);
            };
            for (auto& c : conditions)
                qualify(c);
            std::set<std::string> filtered;
            for (const auto& source : s.sources)
                for (const auto& c : conditions)
                    if (local_guard(c, source.alias))
                        filtered.insert(source.alias);
            std::size_t root = 0;
            for (const auto& c : conditions) {
                auto refs = qualifiers(c);
                if (refs.size() == 1 && !refs.contains("")) {
                    for (std::size_t i = 0; i < s.sources.size(); ++i)
                        if (s.sources[i].alias == *refs.begin())
                            root = i;
                    break;
                }
            }
            q.table = s.sources[root].table;
            q.alias = s.sources[root].alias;
            std::set<std::string> joined{q.alias};
            std::vector<bool> used(conditions.size(), false);
            std::vector<Predicate> base;
            for (std::size_t i = 0; i < conditions.size(); ++i)
                if (qualifiers(conditions[i]) == std::set<std::string>{q.alias}) {
                    auto single = local;
                    single.sources = {s.sources[root]};
                    single.qualified = false;
                    base.push_back(single.predicate(conditions[i]));
                    used[i] = true;
                }
            if (!base.empty())
                q.source_where = conjunction(std::move(base));
            while (joined.size() < s.sources.size()) {
                bool found = false;
                // Prefer a connected filtered source before expanding unfiltered input.
                for (int pass = 0; pass < 2 && !found; ++pass)
                    for (std::size_t i = 0; i < conditions.size() && !found; ++i) {
                        const auto& c = conditions[i];
                        if (used[i] || c.kind != Node::binary || (c.name != "=" && c.name != "==") ||
                            c.args[0].kind != Node::column || c.args[1].kind != Node::column)
                            continue;
                        if (local.expression_type(local.expression(c.args[0])) !=
                            local.expression_type(local.expression(c.args[1])))
                            continue;
                        auto a = c.args[0].qualifier, b = c.args[1].qualifier;
                        if (a.empty() || b.empty() || joined.contains(a) == joined.contains(b))
                            continue;
                        auto next = joined.contains(a) ? b : a;
                        if (pass == 0 && !filtered.contains(next))
                            continue;
                        auto it = std::find_if(s.sources.begin(), s.sources.end(),
                                               [&](const Source& src) { return src.alias == next; });
                        if (it == s.sources.end())
                            throw Error(ErrorCode::schema, "Unknown join alias");
                        q.joins.push_back(
                            {it->table, it->alias, local.expression(c.args[0]), local.expression(c.args[1])});
                        joined.insert(next);
                        used[i] = true;
                        found = true;
                    }
                if (!found)
                    for (const auto& source : s.sources)
                        if (!joined.contains(source.alias)) {
                            q.joins.push_back({source.table, source.alias, literal(std::int64_t{0}),
                                               literal(std::int64_t{0}), true});
                            joined.insert(source.alias);
                            break;
                        }
            }
            for (auto& join : q.joins) {
                std::vector<Predicate> filters;
                auto single = local;
                single.sources = {{join.table, join.alias}};
                single.qualified = false;
                for (std::size_t i = 0; i < conditions.size(); ++i)
                    if (!used[i] && qualifiers(conditions[i]) == std::set<std::string>{join.alias}) {
                        filters.push_back(single.predicate(conditions[i]));
                        used[i] = true;
                    }
                for (const auto& c : conditions)
                    if (!join.cross && qualifiers(c).size() > 1)
                        if (auto guard = local_guard(c, join.alias))
                            filters.push_back(single.predicate(*guard));
                if (!filters.empty())
                    join.right_where = conjunction(std::move(filters));
            }
            std::vector<Predicate> rest;
            for (std::size_t i = 0; i < conditions.size(); ++i)
                if (!used[i])
                    rest.push_back(local.predicate(conditions[i]));
            if (!rest.empty())
                q.where = conjunction(std::move(rest));
        }
    }
}
void project_and_group(const Select& s, const Lowerer& local, Query& q) {
    const auto& schema = local.schema;
    if (s.projection.size() == 1 && s.projection[0].kind == Node::star) {
        if (s.sources.empty())
            throw Error(ErrorCode::schema, "Star requires a source table");
        // Keep SQL FROM order even when the execution join order differs.
        for (const auto& source : s.sources)
            for (const auto& c : table(schema, source.table))
                q.select.push_back(local.qualified ? column(source.alias, c.name) : column(c.name));
    } else
        for (const auto& n : s.projection)
            q.select.push_back(local.expression(n));
    for (const auto& key : s.group) {
        if (key.kind == Node::literal && std::holds_alternative<std::int64_t>(key.value)) {
            auto i = std::get<std::int64_t>(key.value);
            if (i < 1 || std::size_t(i) > q.select.size())
                throw Error(ErrorCode::schema, "GROUP BY ordinal out of range");
            q.group_by.push_back(q.select[std::size_t(i - 1)]);
        } else {
            auto alias = key.kind == Node::column && key.qualifier.empty()
                             ? std::find(s.aliases.begin(), s.aliases.end(), key.name)
                             : s.aliases.end();
            bool input = false;
            if (key.kind == Node::column)
                for (const auto& source : s.sources)
                    for (const auto& c : table(schema, source.table))
                        input |= c.name == key.name;
            q.group_by.push_back(!input && alias != s.aliases.end()
                                     ? q.select[static_cast<std::size_t>(alias - s.aliases.begin())]
                                     : local.expression(key));
        }
    }
    if (s.having)
        q.having = local.predicate(*s.having);
}
void plan_compounds(const Select& s, const Lowerer& lower, const Lowerer& local, Query& q) {
    for (const auto& [op, part] : s.compounds)
        q.compounds.emplace_back(op, std::make_shared<const Query>(lower.query(*part)));
    if (!q.compounds.empty()) {
        std::vector<Query> parts;
        for (const auto& [op, part] : q.compounds) {
            (void)op;
            parts.push_back(*part);
        }
        std::vector<std::vector<std::optional<Type>>> types;
        auto output_types = [&](const Query& part) {
            Schema expanded = lower.schema;
            for (const auto& relation : part.relations)
                expanded[relation.name] = relation.columns;
            Lowerer context{expanded,       lower.registry, lower.parameters,     {}, lower.outer,
                            lower.captures, false,          lower.parameter_types};
            context.types = lower.types;
            if (!part.table.empty())
                context.sources.push_back({part.table, part.alias.empty() ? part.table : part.alias});
            for (const auto& j : part.joins)
                context.sources.push_back({j.table, j.alias});
            std::vector<std::optional<Type>> out;
            for (const auto& e : part.select)
                out.push_back(context.expression_type(e));
            return out;
        };
        types.push_back(output_types(q));
        for (const auto& part : parts)
            types.push_back(output_types(part));
        for (std::size_t i = 0; i < q.select.size(); ++i) {
            bool mixed = false, known = true;
            auto first = types[0][i];
            for (const auto& t : types) {
                if (i >= t.size())
                    throw Error(ErrorCode::schema, "Compound projection widths differ");
                if (!t[i] || !first) {
                    known = false;
                    continue;
                }
                mixed |= *t[i] != *first;
            }
            if (mixed && known) {
                std::vector<Type> numeric;
                for (const auto& t : types)
                    numeric.push_back(*t[i]);
                if (auto common = lower.types->common_type(numeric)) {
                    q.select[i] = local.conversion(std::move(q.select[i]), *common);
                    for (auto& part : parts)
                        part.select[i] = local.conversion(std::move(part.select[i]), *common);
                    continue;
                }
                throw Error(ErrorCode::type, "Compound result types have no common SQL type");
            }
        }
        for (std::size_t i = 0; i < parts.size(); ++i)
            q.compounds[i].second = std::make_shared<const Query>(std::move(parts[i]));
    }
}
void plan_order_limit(const Select& s, const Lowerer& lower, const Lowerer& local, Query& q) {
    for (const auto& [key, descending] : s.order) {
        Expr order;
        if (key.kind == Node::literal && std::holds_alternative<std::int64_t>(key.value)) {
            auto i = std::get<std::int64_t>(key.value);
            if (i < 1 || std::size_t(i) > q.select.size())
                throw Error(ErrorCode::schema, "ORDER BY ordinal out of range");
            order = q.select[std::size_t(i - 1)];
        } else {
            auto alias = key.kind == Node::column && key.qualifier.empty()
                             ? std::find(s.aliases.begin(), s.aliases.end(), key.name)
                             : s.aliases.end();
            order = alias == s.aliases.end() ? local.expression(key)
                                             : q.select[static_cast<std::size_t>(alias - s.aliases.begin())];
        }
        if (!s.compounds.empty()) {
            std::optional<std::size_t> index;
            if (key.kind == Node::literal && std::holds_alternative<std::int64_t>(key.value))
                index = static_cast<std::size_t>(std::get<std::int64_t>(key.value) - 1);
            else
                for (std::size_t i = 0; i < s.projection.size(); ++i)
                    if (key.kind == Node::column &&
                        ((key.qualifier.empty() && s.aliases[i] == key.name) ||
                         (s.projection[i].kind == Node::column && s.projection[i].name == key.name &&
                          s.projection[i].qualifier == key.qualifier))) {
                        index = i;
                        break;
                    }
            if (!index)
                unsupported("Compound ORDER BY requires an output column or ordinal");
            order = column(std::to_string(*index));
        }
        q.order_by.push_back(Order{std::move(order), descending});
    }
    if (s.offset) {
        auto v = lower.constant(*s.offset);
        auto n = std::get_if<std::int64_t>(&v);
        if (!n || *n < 0)
            unsupported("OFFSET requires a nonnegative integer");
        q.offset = static_cast<std::size_t>(*n);
    }
    if (s.limit) {
        auto v = lower.constant(*s.limit);
        auto n = std::get_if<std::int64_t>(&v);
        if (!n || *n < 0)
            unsupported("LIMIT requires a nonnegative integer");
        q.limit = static_cast<std::size_t>(*n);
    }
}
} // namespace
Query Lowerer::query_body(const Select& s) const {
    Lowerer local = *this;
    local.sources = s.sources;
    local.qualified = s.sources.size() > 1;
    Query q;
    plan_from(s, local, q);
    project_and_group(s, local, q);
    plan_compounds(s, *this, local, q);
    plan_order_limit(s, *this, local, q);
    q.repeatable = local.repeatable(s);
    return q;
}
} // namespace coresql::sql::detail
