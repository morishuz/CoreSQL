#include "ast.hpp"
#include <bit>
namespace coresql::sql {
namespace {
bool same_parameters(const std::vector<std::pair<Type, bool>>& a, std::span<const Value> b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].first != type_of(b[i]) || a[i].second != is_null(b[i]))
            return false;
    return true;
}
bool templatable(const detail::Select& s) {
    if (!s.ctes.empty() || !s.compounds.empty() || s.sources.size() > 1 ||
        (!s.sources.empty() && s.sources[0].query))
        return false;
    auto local = [&](auto&& self, const detail::Node& n) -> bool {
        if (n.query)
            return false;
        return std::all_of(n.args.begin(), n.args.end(), [&](const auto& arg) { return self(self, arg); });
    };
    for (const auto& n : s.projection)
        if (!local(local, n))
            return false;
    for (const auto& n : s.group)
        if (!local(local, n))
            return false;
    for (const auto& [n, descending] : s.order)
        if (!local(local, n))
            return false;
    return (!s.where || local(local, *s.where)) && (!s.having || local(local, *s.having)) &&
           (!s.limit || local(local, *s.limit)) && (!s.offset || local(local, *s.offset));
}
Query instantiate(Query query, const detail::Statement& statement, const Schema& schema,
                  const Registry& registry, const TypeAdapters& adapters, std::span<const Value> values) {
    auto expr = [&](auto&& self, Expr& e) -> void {
        if (e.kind == Expr::Kind::parameter && e.name.starts_with("\x01sql.parameter.")) {
            e = literal(values[std::stoull(e.name.substr(15))]);
            return;
        }
        for (auto& arg : e.arguments)
            self(self, arg);
    };
    auto pred = [&](auto&& self, Predicate& p) -> void {
        expr(expr, p.left);
        expr(expr, p.right);
        for (auto& child : p.children)
            self(self, child);
    };
    for (auto& e : query.select)
        expr(expr, e);
    for (auto& e : query.group_by)
        expr(expr, e);
    for (auto& o : query.order_by)
        expr(expr, o.expression);
    if (query.where)
        pred(pred, *query.where);
    if (query.having)
        pred(pred, *query.having);
    detail::Lowerer lower{schema, registry, values, {}};
    lower.types = &adapters;
    auto count = [&](const detail::Node& n) {
        const auto value = lower.constant(n);
        const auto* i = std::get_if<std::int64_t>(&value);
        if (!i || *i < 0)
            throw Error(ErrorCode::unsupported, "LIMIT/OFFSET requires a nonnegative INTEGER");
        return static_cast<std::size_t>(*i);
    };
    if (statement.query->limit)
        query.limit = count(*statement.query->limit);
    if (statement.query->offset)
        query.offset = count(*statement.query->offset);
    return query;
}
bool cacheable(const Query& q, const Schema& schema, std::span<const Value> parameters) {
    if (!q.repeatable || q.join || !q.joins.empty() || !q.relations.empty() || !q.compounds.empty() ||
        q.search)
        return false;
    // One entry, <=256 schema columns, <=1024 expression nodes and <=64 KiB of
    // names/value payload. Container overhead is additional. No bound pointers.
    std::size_t bytes = 65536, nodes = 0, columns = 0;
    auto take = [&](std::size_t n) {
        if (n > bytes)
            return false;
        bytes -= n;
        return true;
    };
    auto value = [&](const Value& v) {
        const auto type = type_of(v);
        if (!take(sizeof(Value) + type.id.size() + type.parameters.size()))
            return false;
        if (const auto* s = std::get_if<std::string>(&v))
            return take(s->size());
        if (const auto* o = std::get_if<Opaque>(&v))
            return take(o->bytes().size());
        return true;
    };
    for (const auto& p : parameters)
        if (!value(p))
            return false;
    for (const auto& [name, fields] : schema) {
        if (!take(name.size()))
            return false;
        for (const auto& field : fields)
            if (++columns > 256 ||
                !take(sizeof(Column) + field.name.size() + field.type.id.size() +
                      field.type.parameters.size() + field.index.size()) ||
                (field.default_value && !value(*field.default_value)))
                return false;
    }
    auto expr = [&](auto&& self, const Expr& e) -> bool {
        if (++nodes > 1024 || e.subquery || !take(sizeof(Expr) + e.name.size() + e.qualifier.size()))
            return false;
        if (e.kind == Expr::Kind::literal && !value(e.value))
            return false;
        for (const auto& child : e.arguments)
            if (!self(self, child))
                return false;
        return true;
    };
    auto pred = [&](auto&& self, const Predicate& p) -> bool {
        if (!expr(expr, p.left) || !expr(expr, p.right) || !take(p.extractor.size()))
            return false;
        for (const auto& child : p.children)
            if (!self(self, child))
                return false;
        return true;
    };
    for (const auto& e : q.select)
        if (!expr(expr, e))
            return false;
    for (const auto& e : q.group_by)
        if (!expr(expr, e))
            return false;
    for (const auto& o : q.order_by)
        if (!expr(expr, o.expression))
            return false;
    return (!q.where || pred(pred, *q.where)) && (!q.having || pred(pred, *q.having)) &&
           (!q.source_where || pred(pred, *q.source_where));
}
} // namespace
Query Connection::prepare_query(Transaction& tx, const Statement& statement,
                                std::span<const Value> parameters, std::vector<std::string>& names) {
    const auto& s = *statement.parsed_;
    if (s.kind != detail::Statement::select)
        throw Error(ErrorCode::unsupported, "Controlled queries require SELECT");
    if (parameters.size() != s.parameters)
        throw Error(ErrorCode::type, "SQL parameter count differs");
    auto schema = tx.schema();
    if (query_cache_ && query_cache_->statement == statement.parsed_ && query_cache_->schema == schema &&
        same_parameters(query_cache_->parameters, parameters)) {
        names = query_cache_->names;
        ++query_cache_stats_.hits;
        return instantiate(query_cache_->query, s, schema, registry_, adapters_, parameters);
    }
    ++query_cache_stats_.misses;
    query_cache_.reset();
    detail::Lowerer lower{schema, registry_, parameters, {}};
    lower.types = &adapters_;
    lower.parameterize = query_cache_enabled_ && templatable(*s.query) && lower.repeatable(*s.query);
    std::vector<Column> columns;
    auto query = lower.query(*s.query, &columns);
    for (const auto& column : columns)
        names.push_back(column.name);
    if (lower.parameterize && cacheable(query, schema, parameters)) {
        std::vector<std::pair<Type, bool>> signature;
        for (const auto& value : parameters)
            signature.emplace_back(type_of(value), is_null(value));
        query_cache_ = CachedQuery{statement.parsed_, schema, std::move(signature), query, names};
    }
    return lower.parameterize ? instantiate(std::move(query), s, schema, registry_, adapters_, parameters)
                              : std::move(query);
}
} // namespace coresql::sql
