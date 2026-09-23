#include "ast.hpp"
#include <set>
namespace coresql::sql::detail {
UpsertPlan prepare_upsert(Transaction& tx, const Statement& s, const Schema& schema) {
    const auto& columns = schema.at(s.table);
    auto position = [&](const std::string& name) {
        const auto found =
            std::find_if(columns.begin(), columns.end(), [&](const Column& c) { return c.name == name; });
        if (found == columns.end())
            throw Error(ErrorCode::schema, "Unknown UPSERT column: " + name);
        return static_cast<std::size_t>(found - columns.begin());
    };
    UpsertPlan plan;
    auto& keys = plan.keys;
    for (const auto& column : columns)
        if (column.primary_key)
            keys.push_back({column.name});
    for (const auto& index : tx.indexes(s.table))
        if (index.unique)
            keys.push_back(index.columns);
    if (!s.conflict_columns.empty()) {
        std::set<std::string> requested;
        for (const auto& c : s.conflict_columns) {
            position(c);
            if (!requested.insert(c).second)
                throw Error(ErrorCode::schema, "Duplicate UPSERT target column");
        }
        auto found = std::find_if(keys.begin(), keys.end(), [&](const auto& key) {
            return std::set<std::string>(key.begin(), key.end()) == requested;
        });
        if (found == keys.end())
            throw Error(ErrorCode::schema, "UPSERT target is not a primary or UNIQUE key");
        keys = {*found};
    }
    return plan;
}
bool upsert(Transaction& tx, const Statement& s, const Registry& registry, std::span<const Value> parameters,
            const TypeAdapters& adapters, const Schema& schema, const UpsertPlan& plan, Row& row) {
    const auto& columns = schema.at(s.table);
    auto position = [&](const std::string& name) {
        const auto found =
            std::find_if(columns.begin(), columns.end(), [&](const Column& c) { return c.name == name; });
        if (found == columns.end())
            throw Error(ErrorCode::schema, "Unknown UPSERT column: " + name);
        return static_cast<std::size_t>(found - columns.begin());
    };
    Lowerer lower{schema, registry, parameters, {{s.table, s.table}}};
    lower.types = &adapters;
    auto incoming = [&](Node n) {
        auto visit = [&](auto&& self, Node& node) -> void {
            if (node.query)
                throw Error(ErrorCode::unsupported, "UPSERT assignments and WHERE must be row-local");
            if (node.kind == Node::column && node.qualifier == "excluded") {
                const auto i = position(node.name);
                node = Node{};
                node.value = row[i];
            }
            for (auto& child : node.args)
                self(self, child);
        };
        visit(visit, n);
        return n;
    };
    Query update{};
    update.table = s.table;
    std::vector<std::size_t> assigned;
    std::set<std::string> names;
    for (const auto& [name, node] : s.assignments) {
        if (!names.insert(name).second)
            throw Error(ErrorCode::schema, "Duplicate UPSERT assignment");
        const auto i = position(name);
        assigned.push_back(i);
        auto e = lower.stored_expression(incoming(node), columns[i].type);
        if (e.kind == Expr::Kind::literal && is_null(e.value))
            e.value = Null(columns[i].type);
        update.select.push_back(std::move(e));
    }
    if (s.where)
        update.where = lower.predicate(incoming(*s.where));
    if (s.conflict == Statement::do_update) {
        // Bind even when no key conflicts, before validating the proposed row.
        update.limit = 0;
        const auto shape = tx.query(update);
        for (std::size_t i = 0; i < assigned.size(); ++i)
            if (shape.types[i] != columns[assigned[i]].type)
                throw Error(ErrorCode::type, "UPSERT assignment type differs");
    }
    // CHECK/NOT NULL failures are never swallowed by conflict handling.
    tx.validate_row(s.table, row);
    std::vector<Assignment> assignments;
    for (std::size_t i = 0; i < assigned.size(); ++i)
        assignments.push_back({columns[assigned[i]].name, std::move(update.select[i])});
    for (const auto& key : plan.keys) {
        std::vector<Predicate> predicates;
        for (const auto& name : key) {
            const auto i = position(name);
            if (is_null(row[i])) {
                predicates.clear();
                break;
            }
            predicates.push_back({column(name), Compare::equal, literal(row[i])});
        }
        if (predicates.empty())
            continue;
        auto equal = all_of(std::move(predicates));
        if (s.conflict == Statement::do_nothing) {
            Query lookup{};
            lookup.table = s.table;
            lookup.select = {literal(std::int64_t{1})};
            lookup.where = std::move(equal);
            lookup.limit = 1;
            if (!tx.query(lookup).rows.empty())
                return false;
        } else {
            auto result =
                tx.update_if(s.table, assignments, std::move(equal), update.where, !s.returning.empty());
            if (!result.matched)
                continue;
            if (!result.updated)
                return false;
            if (!s.returning.empty())
                row = std::move(result.returning.rows.front());
            return true;
        }
    }
    tx.insert(s.table, row);
    return true;
}
} // namespace coresql::sql::detail
