#include "lower.hpp"
#include "coresql/date.hpp"
#include "coresql/decimal.hpp"
#include <set>

namespace coresql::sql::detail {
namespace {
std::optional<Type> column_affinity(const Node& n, std::optional<Type> type) {
    if (n.kind == Node::function) {
        if (n.name == "sql.cast_numeric" || n.name == "sql.cast_integer")
            return integer();
        if (n.name == "sql.cast_real")
            return real();
        if (n.name == "sql.cast_text")
            return text();
    }
    if ((n.kind == Node::column || n.kind == Node::captured_column) && type &&
        (*type == integer() || *type == real() || *type == text()))
        return type;
    return {};
}
std::string equality_function(std::optional<Type> left, std::optional<Type> right = {}) {
    auto numeric = [](const std::optional<Type>& t) { return t && (*t == integer() || *t == real()); };
    return numeric(left) || numeric(right) ? "sql.equal_numeric"
           : left || right                 ? "sql.equal_text"
                                           : "sql.equal";
}
// Bare NULL has no domain identity. Give it the other operand's domain type
// before registered comparison functions infer their argument types.
void domain_nulls(std::initializer_list<Expr*> args, const Lowerer& lower) {
    auto bare_null = [](const Expr* e) { return e->kind == Expr::Kind::literal && is_null(e->value); };
    if (std::none_of(args.begin(), args.end(), bare_null))
        return;
    std::optional<Type> domain;
    for (auto e : args) {
        if (e->kind == Expr::Kind::literal && is_null(e->value))
            continue;
        auto t = lower.expression_type(*e);
        if (t && !scalar_type(*t)) {
            domain = t;
            break;
        }
    }
    if (domain)
        for (auto e : args)
            if (e->kind == Expr::Kind::literal && is_null(e->value))
                e->value = Null(*domain);
}
} // namespace
Expr Lowerer::expression(const Node& n) const {
    switch (n.kind) {
    case Node::literal:
        return literal(n.value);
    case Node::captured_column:
        return parameter(n.name);
    case Node::parameter:
        if (n.position >= parameters.size())
            throw Error(ErrorCode::type, "Missing SQL parameter");
        return literal(parameters[n.position]);
    case Node::column: {
        std::vector<const Source*> matches;
        for (const auto& s : sources) {
            if (!n.qualifier.empty() && n.qualifier != s.alias)
                continue;
            const auto& cols = table(schema, s.table);
            if (n.name == "rowid" ||
                std::any_of(cols.begin(), cols.end(), [&](const Column& c) { return c.name == n.name; }))
                matches.push_back(&s);
        }
        if (matches.size() > 1)
            throw Error(ErrorCode::schema, "Ambiguous SQL column: " + n.name);
        if (matches.empty()) {
            if (!outer || !captures)
                throw Error(ErrorCode::schema, "Unknown SQL column: " + n.name);
            auto value = outer->expression(n);
            auto name = "sql.outer." + std::to_string(captures->size());
            captures->emplace_back(name, std::move(value));
            return parameter(name);
        }
        if (n.name == "rowid") {
            const auto& cols = table(schema, matches[0]->table);
            bool named =
                std::any_of(cols.begin(), cols.end(), [](const Column& c) { return c.name == "rowid"; });
            if (!named) {
                if (sources.size() != 1)
                    unsupported("Joined rowid is unsupported");
                if (matches[0]->table.starts_with("\x01"))
                    unsupported("Derived relations have no rowid");
                auto pk =
                    std::find_if(cols.begin(), cols.end(), [](const Column& c) { return c.primary_key; });
                if (pk != cols.end()) {
                    if (pk->type != integer())
                        unsupported("rowid on a non-integer primary key table is unsupported");
                    return column(pk->name);
                }
                return row_id();
            }
        }
        return qualified ? column(matches[0]->alias, n.name) : column(n.name);
    }
    case Node::star:
        unsupported("Star is only valid as the complete projection or count(*)");
    case Node::membership: {
        auto value = expression(n.args[0]);
        auto left_affinity = column_affinity(n.args[0], expression_type(value));
        auto equality = decimals::is_decimal(expression_type(value).value_or(integer()))
                            ? "decimal.equal"
                            : equality_function(left_affinity);
        if (n.query) {
            std::vector<std::pair<std::string, Expr>> bindings;
            Lowerer nested{schema, registry, parameters, {}, this, &bindings};
            nested.ctes = ctes;
            nested.relation_id = relation_id;
            auto q = nested.query(*n.query);
            auto candidate_type = expression_type(scalar_subquery(q, bindings));
            if (decimals::is_decimal(candidate_type.value_or(integer())) ||
                decimals::is_decimal(expression_type(value).value_or(integer()))) {
                equality = "decimal.equal";
                value = decimal_literal(n.args[0], std::move(value));
            } else if (n.query->compounds.empty() && n.query->projection.size() == 1)
                equality =
                    equality_function(left_affinity, column_affinity(n.query->projection[0], candidate_type));
            return membership(std::move(value), std::move(q), equality, std::move(bindings));
        }
        std::vector<Expr> candidates;
        for (std::size_t i = 1; i < n.args.size(); ++i)
            candidates.push_back(expression(n.args[i]));
        if (decimals::is_decimal(expression_type(value).value_or(integer())) ||
            std::any_of(candidates.begin(), candidates.end(), [&](const Expr& e) {
                return decimals::is_decimal(expression_type(e).value_or(integer()));
            })) {
            value = decimal_literal(n.args[0], std::move(value));
            for (std::size_t i = 0; i < candidates.size(); ++i)
                candidates[i] = decimal_literal(n.args[i + 1], std::move(candidates[i]));
            equality = "decimal.equal";
        }
        return membership(std::move(value), std::move(candidates), equality);
    }
    case Node::subquery:
    case Node::exists: {
        std::vector<std::pair<std::string, Expr>> bindings;
        Lowerer nested{schema, registry, parameters, {}, this, &bindings};
        nested.ctes = ctes;
        nested.relation_id = relation_id;
        auto q = nested.query(*n.query);
        return n.kind == Node::exists ? exists(std::move(q), std::move(bindings))
                                      : scalar_subquery(std::move(q), std::move(bindings));
    }
    case Node::case_when:
    case Node::case_match: {
        std::size_t first = n.kind == Node::case_match ? 1 : 0;
        bool fallback = (n.args.size() - first) % 2 != 0;
        auto end = n.args.size() - (fallback ? 1 : 0);
        std::vector<std::pair<Expr, Expr>> branches;
        for (std::size_t i = first; i < end; i += 2)
            branches.emplace_back(expression(n.args[i]), expression(n.args[i + 1]));
        std::optional<Expr> otherwise;
        if (fallback)
            otherwise = expression(n.args.back());
        std::vector<Expr*> arms;
        for (auto& branch : branches)
            arms.push_back(&branch.second);
        if (otherwise)
            arms.push_back(&*otherwise);
        unify(arms);
        if (!first)
            for (auto& branch : branches)
                branch.first = truth(std::move(branch.first));
        if (first) {
            auto base = expression(n.args[0]);
            bool decimal = decimals::is_decimal(expression_type(base).value_or(integer())) ||
                           std::any_of(branches.begin(), branches.end(), [&](const auto& branch) {
                               return decimals::is_decimal(expression_type(branch.first).value_or(integer()));
                           });
            auto equality = decimal ? "decimal.equal"
                                    : equality_function(column_affinity(n.args[0], expression_type(base)));
            if (decimal) {
                base = decimal_literal(n.args[0], std::move(base));
                for (std::size_t i = 0; i < branches.size(); ++i)
                    branches[i].first = decimal_literal(n.args[first + 2 * i], std::move(branches[i].first));
            }
            return choose(std::move(base), equality, std::move(branches), std::move(otherwise));
        }
        return choose(std::move(branches), std::move(otherwise));
    }
    case Node::unary:
        if (n.name == "-") {
            auto e = expression(n.args[0]);
            auto name = decimals::is_decimal(expression_type(e).value_or(integer())) ? "decimal.negate"
                                                                                     : "sql.numeric_negate";
            return call(name, {std::move(e)});
        }
        return call(function_name(n.name),
                    {n.name == "not" ? truth(expression(n.args[0])) : expression(n.args[0])});
    case Node::binary: {
        if (n.name == "and" || n.name == "or") {
            std::vector<Expr> level;
            for (const auto& a : n.args)
                level.push_back(truth(expression(a)));
            while (level.size() > 1) {
                std::vector<Expr> next;
                for (std::size_t i = 0; i < level.size(); i += 2) {
                    if (i + 1 == level.size())
                        next.push_back(std::move(level[i]));
                    else
                        next.push_back(
                            call(function_name(n.name), {std::move(level[i]), std::move(level[i + 1])}));
                }
                level = std::move(next);
            }
            return std::move(level[0]);
        }
        if (n.name == "between") {
            std::vector<Expr> args;
            for (const auto& a : n.args)
                args.push_back(expression(a));
            bool decimal = std::any_of(args.begin(), args.end(), [&](const Expr& e) {
                return decimals::is_decimal(expression_type(e).value_or(integer()));
            });
            if (decimal)
                for (std::size_t i = 0; i < args.size(); ++i)
                    args[i] = decimal_literal(n.args[i], std::move(args[i]));
            domain_nulls({&args[0], &args[1], &args[2]}, *this);
            if (decimal)
                return call("decimal.between", std::move(args));
            auto type = expression_type(args[0]);
            if (n.args[0].kind == Node::column && type &&
                (*type == integer() || *type == real() || *type == text()))
                for (std::size_t i = 1; i < 3; ++i) {
                    auto other = expression_type(args[i]);
                    if (other && *other != *type)
                        args[i] = call(*type == text() ? "sql.cast_text" : "sql.numeric_affinity",
                                       {std::move(args[i])});
                }
            return call("sql.between", std::move(args));
        }
        auto left = expression(n.args[0]), right = expression(n.args[1]);
        if ((comparison(n.name) || n.name == "+" || n.name == "-" || n.name == "*" || n.name == "/") &&
            (decimals::is_decimal(expression_type(left).value_or(integer())) ||
             decimals::is_decimal(expression_type(right).value_or(integer())))) {
            left = decimal_literal(n.args[0], std::move(left));
            right = decimal_literal(n.args[1], std::move(right));
            domain_nulls({&left, &right}, *this);
            auto op = function_name(n.name);
            op = op.substr(op.find_last_of('.') + 1);
            if (op.starts_with("numeric_"))
                op = op.substr(8);
            return call("decimal." + op, {std::move(left), std::move(right)});
        }
        // Literal text concatenation cannot fail with a value/type error. Expose
        // its value to the core's range-index selection after parameter binding.
        // Do not eagerly evaluate user callbacks or potentially failing arithmetic.
        if (n.name == "||" && left.kind == Expr::Kind::literal && right.kind == Expr::Kind::literal) {
            auto a = std::get_if<std::string>(&left.value), b = std::get_if<std::string>(&right.value);
            if (a && b)
                return literal(*a + *b);
        }
        if (n.name == "like") {
            auto lt = expression_type(left), rt = expression_type(right);
            if (lt && *lt != text() && scalar_type(*lt))
                left = call("sql.cast_text", {std::move(left)});
            if (rt && *rt != text() && scalar_type(*rt))
                right = call("sql.cast_text", {std::move(right)});
        }
        if (comparison(n.name)) {
            domain_nulls({&left, &right}, *this);
            auto lt = expression_type(left), rt = expression_type(right);
            if (lt && rt && *lt != *rt && scalar_type(*lt) && scalar_type(*rt)) {
                auto affinity = [&](Expr& value, const Type& source_type) {
                    value = call(source_type == text() ? "sql.cast_text" : "sql.numeric_affinity",
                                 {std::move(value)});
                };
                auto la = column_affinity(n.args[0], lt), ra = column_affinity(n.args[1], rt);
                if (la && *la != text())
                    affinity(right, *la);
                else if (ra && *ra != text())
                    affinity(left, *ra);
                else if (la)
                    affinity(right, *la);
                else if (ra)
                    affinity(left, *ra);
            }
        }
        // Preserve registered integer operations when conversion is unnecessary.
        // Mixed/text/dynamic operands still use the SQL conversion functions.
        if ((n.name == "+" || n.name == "*" || n.name == "&") && expression_type(left) == integer() &&
            expression_type(right) == integer())
            return call(n.name == "+"   ? "integer.add"
                        : n.name == "*" ? "integer.multiply"
                                        : "integer.bitand",
                        {std::move(left), std::move(right)});
        return call(function_name(n.name), {std::move(left), std::move(right)});
    }
    case Node::function: {
        if (n.name == "sql.cast_decimal")
            return stored_expression(n.args.at(0), type_of(n.value), true);
        bool reduction = false;
        try {
            registry.aggregate(n.name);
            reduction = true;
        } catch (const Error&) {
        }
        std::vector<Expr> args;
        if (n.args.size() == 1 && n.args[0].kind == Node::star) {
            if (n.name != "count")
                unsupported("Only count accepts star");
        } else
            for (const auto& a : n.args)
                args.push_back(expression(a));
        if ((n.name == "date.year" || n.name == "text.substring") && !args.empty() &&
            args[0].kind == Expr::Kind::literal && is_null(args[0].value))
            args[0] = literal(Null(n.name == "date.year" ? dates::type() : text()));
        if (n.distinct && (!reduction || args.size() != 1))
            throw Error(ErrorCode::schema, "DISTINCT requires a single aggregate argument");
        if (n.name == "length" || n.name == "like")
            for (auto& arg : args) {
                auto type = expression_type(arg);
                if (type && *type != text() && scalar_type(*type))
                    arg = call("sql.cast_text", {std::move(arg)});
            }
        // SQLite's scalar like(pattern, value) reverses the infix operand order.
        if (n.name == "like" && args.size() == 2)
            std::swap(args[0], args[1]);
        if (n.name == "coalesce") {
            if (args.size() < 2)
                throw Error(ErrorCode::type, "COALESCE needs at least two operands");
            std::vector<Expr*> arms;
            for (auto& arg : args)
                arms.push_back(&arg);
            unify(arms);
            return coalesce(std::move(args));
        }
        if (n.name == "sql.cast_date" && args.size() == 1 && args[0].kind == Expr::Kind::literal &&
            is_null(args[0].value))
            args[0] = literal(Null(dates::type()));
        auto name = n.name;
        if (args.size() == 1 && decimals::is_decimal(expression_type(args[0]).value_or(integer()))) {
            if (name == "sum" || name == "avg")
                name = "decimal." + name;
            else if (name == "abs")
                name = "decimal.abs";
        }
        if (reduction && args.size() == 1) {
            auto type = expression_type(args[0]);
            if (type && (((name == "sum" || name == "avg") && (*type == text() || *type == any_type())) ||
                         (name == "group_concat" && (*type == real() || *type == any_type()))))
                name = "sql." + name;
        }
        return reduction ? aggregate(name, std::move(args), n.distinct)
                         : call(function_name(name), std::move(args));
    }
    }
    unsupported("Unknown SQL expression");
}
Predicate Lowerer::predicate(const Node& n) const {
    if (n.kind == Node::binary) {
        if (n.name == "and" || n.name == "or") {
            std::vector<Predicate> children;
            for (const auto& a : n.args)
                children.push_back(predicate(a));
            return n.name == "and" ? conjunction(std::move(children)) : any_of(std::move(children));
        }
        if (n.name == "between") {
            auto e = expression(n.args[0]), lo = expression(n.args[1]), hi = expression(n.args[2]);
            auto simple = [](const Expr& x) {
                return x.kind == Expr::Kind::column || x.kind == Expr::Kind::literal;
            };
            if (simple(e) && simple(lo) && simple(hi) && expression_type(e) == expression_type(lo) &&
                expression_type(e) == expression_type(hi))
                return all_of({{e, Compare::greater_equal, lo}, {e, Compare::less_equal, hi}});
            return {expression(n), Compare::not_equal, literal(std::int64_t{0})};
        }
        if (auto c = comparison(n.name)) {
            auto left = expression(n.args[0]), right = expression(n.args[1]);
            auto lt = expression_type(left), rt = expression_type(right);
            if (lt && rt && *lt == *rt)
                return {std::move(left), *c, std::move(right)};
            return {expression(n), Compare::not_equal, literal(std::int64_t{0})};
        }
    }
    if (n.kind == Node::unary && n.name == "not")
        return not_(predicate(n.args[0]));
    return {truth(expression(n)), Compare::not_equal, literal(std::int64_t{0})};
}
Value Lowerer::constant(const Node& n) const {
    return evaluate_constant(expression(n), registry);
}
} // namespace coresql::sql::detail
