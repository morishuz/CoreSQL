#include "bound.hpp"
#include "index.hpp"
#include "scan.hpp"
#include <set>

namespace coresql::detail {
namespace {
using namespace execution;
std::size_t position(const Table& table, const std::string& name) {
    auto found = std::find_if(table.columns.begin(), table.columns.end(),
                              [&](const Column& c) { return c.name == name; });
    if (found == table.columns.end())
        fail(ErrorCode::schema, "Unknown constraint column: " + name);
    return static_cast<std::size_t>(found - table.columns.begin());
}
std::vector<std::size_t> positions(const Table& table, const std::vector<std::string>& names) {
    if (names.empty())
        fail(ErrorCode::schema, "Foreign key needs columns");
    std::set<std::string> seen;
    std::vector<std::size_t> result;
    for (const auto& name : names) {
        if (!seen.insert(name).second)
            fail(ErrorCode::schema, "Duplicate foreign-key column");
        result.push_back(position(table, name));
    }
    return result;
}
std::vector<BoundExpr> checks(const Table& table, const Registry& registry) {
    std::vector<BoundExpr> result;
    for (const auto& check : table.constraints.checks) {
        validate_check_expression(check.expression);
        auto expression = bind(check.expression, table, registry);
        if (expression.type != integer())
            fail(ErrorCode::type, "CHECK must return INTEGER or typed INTEGER NULL");
        result.push_back(std::move(expression));
    }
    return result;
}
void check_row(const Table& table, const Row& row, const Registry& registry,
               const std::vector<BoundExpr>& bound) {
    for (std::size_t i = 0; i < bound.size(); ++i) {
        auto result = bound[i].evaluate(row, registry);
        if (!is_null(result) && std::get<std::int64_t>(result) == 0)
            fail(ErrorCode::constraint, "CHECK failed: " + table.constraints.checks[i].name);
    }
}
struct Reference {
    const ForeignKey& declaration;
    const Table& parent;
    std::vector<std::size_t> child_columns, parent_columns;
    const OrderedIndex* index = nullptr;
    bool primary = false;
    Reference(const ForeignKey& key, const Table& child, const Tables& tables)
        : declaration(key), parent(*require_table(tables, key.referenced_table)),
          child_columns(positions(child, key.columns)),
          parent_columns(positions(parent, key.referenced_columns)) {
        if (child_columns.size() != parent_columns.size())
            fail(ErrorCode::schema, "Foreign-key widths differ");
        for (std::size_t i = 0; i < child_columns.size(); ++i)
            if (child.columns[child_columns[i]].type != parent.columns[parent_columns[i]].type)
                fail(ErrorCode::type, "Foreign-key column types differ");
        primary = parent.primary && parent_columns.size() == 1 && parent.primary->column == parent_columns[0];
        for (const auto& candidate : parent.ordered)
            if (candidate->definition.unique && candidate->definition.columns == key.referenced_columns) {
                index = candidate.get();
                break;
            }
        if (!primary && !index)
            fail(ErrorCode::schema, "Foreign key requires an exact primary or UNIQUE parent key");
    }
    void validate(const Row& row, const Registry& registry, bool self_insert = false) const {
        Row key;
        for (auto c : child_columns) {
            if (is_null(row[c]))
                return; // MATCH SIMPLE
            key.push_back(row[c]);
        }
        if (self_insert) {
            bool same = true;
            for (std::size_t i = 0; i < key.size(); ++i) {
                const auto& value = row[parent_columns[i]];
                if (is_null(value) || !Comparison(registry, parent.columns[parent_columns[i]].type, true)
                                           .equal(key[i], value)) {
                    same = false;
                    break;
                }
            }
            if (same)
                return;
        }
        if (primary ? bool(lookup(parent, key[0])) : !index->equal(key).empty())
            return;
        fail(ErrorCode::constraint, "Foreign key failed: " + declaration.name);
    }
};
void names(const TableConstraints& constraints) {
    std::set<std::string> seen;
    auto add = [&](const auto& items) {
        for (const auto& item : items)
            if (item.name.empty() || !seen.insert(item.name).second)
                fail(ErrorCode::schema, "Empty or duplicate constraint name");
    };
    add(constraints.checks);
    add(constraints.foreign_keys);
}
} // namespace
void validate_check_expression(const Expr& expression) {
    std::size_t nodes = 0;
    auto visit = [&](auto&& self, const Expr& e, unsigned depth) -> void {
        if (++nodes > 4096 || depth > 64)
            throw Error(ErrorCode::schema, "CHECK expression is too large");
        if (e.subquery || !e.qualifier.empty() || !e.parameters.empty() || e.distinct ||
            (e.kind != Expr::Kind::column && e.kind != Expr::Kind::literal && e.kind != Expr::Kind::call &&
             e.kind != Expr::Kind::conditional && e.kind != Expr::Kind::coalesce &&
             e.kind != Expr::Kind::membership))
            throw Error(ErrorCode::unsupported, "CHECK requires row-local scalar expressions");
        for (const auto& child : e.arguments)
            self(self, child, depth + 1);
    };
    visit(visit, expression, 0);
}
void validate_constraints(const Tables& tables, const Registry& registry, const std::string& changed) {
    for (const auto& [name, table] : tables) {
        names(table->constraints);
        if (changed.empty() || name == changed) {
            const auto bound = checks(*table, registry);
            if (!bound.empty())
                visit_table_rows(*table, [&](auto, const Row& row, auto) {
                    check_row(*table, row, registry, bound);
                    return true;
                });
        }
        for (const auto& foreign : table->constraints.foreign_keys) {
            Reference reference(foreign, *table, tables);
            if (changed.empty() || name == changed || foreign.referenced_table == changed)
                visit_table_rows(*table, [&](auto, const Row& row, auto) {
                    reference.validate(row, registry);
                    return true;
                });
        }
    }
}
void validate_insert_constraints(const Tables& tables, const std::string& name, const Row& row,
                                 const Registry& registry) {
    const auto& table = *require_table(tables, name);
    check_row(table, row, registry, checks(table, registry));
    for (const auto& key : table.constraints.foreign_keys)
        Reference(key, table, tables).validate(row, registry, key.referenced_table == name);
}
void validate_replacement_constraints(const Tables& tables, const std::string& name,
                                      const std::shared_ptr<Table>& replacement, const Registry& registry) {
    const auto& original = *require_table(tables, name);
    auto changed_rows = [&](const auto& validate) {
        for (const auto& [id, chunk] : replacement->chunks) {
            const auto before = original.chunks.find(id);
            if (before != original.chunks.end() && before->second == chunk)
                continue;
            const auto pin = chunk.pin();
            for (const auto& row : pin->rows)
                validate(row);
        }
    };
    const auto bound = checks(*replacement, registry);
    if (!bound.empty())
        changed_rows([&](const Row& row) { check_row(*replacement, row, registry, bound); });
    bool needed = false;
    for (const auto& [other, table] : tables)
        for (const auto& key : table->constraints.foreign_keys)
            needed |= other == name || key.referenced_table == name;
    if (!needed)
        return;
    auto candidate = tables;
    candidate[name] = replacement;
    for (const auto& [child, table] : candidate)
        for (const auto& foreign : table->constraints.foreign_keys)
            if (child == name || foreign.referenced_table == name) {
                Reference reference(foreign, *table, candidate);
                if (child == name && foreign.referenced_table != name)
                    changed_rows([&](const Row& row) { reference.validate(row, registry); });
                else
                    visit_table_rows(*table, [&](auto, const Row& row, auto) {
                        reference.validate(row, registry);
                        return true;
                    });
            }
}
} // namespace coresql::detail
namespace coresql {
void Transaction::set_constraints(const std::string& name, TableConstraints constraints) {
    auto owner = active();
    auto candidate = staged_->tables;
    auto& table = detail::require_table(candidate, name);
    table = std::make_shared<detail::Table>(*table);
    table->constraints = std::move(constraints);
    detail::validate_constraints(candidate, owner->registry, name);
    staged_->tables = std::move(candidate);
    dirty_ = true;
}
TableConstraints Transaction::constraints(const std::string& name) const {
    active();
    return detail::require_table(staged_->tables, name)->constraints;
}
} // namespace coresql

namespace coresql {
void Transaction::validate_row(const std::string& name, const Row& row) const {
    auto owner = active();
    const auto& table = *detail::require_table(staged_->tables, name);
    if (row.size() != table.columns.size())
        throw Error(ErrorCode::schema, "Row width differs from schema");
    for (std::size_t i = 0; i < row.size(); ++i)
        detail::validate_stored(row[i], table.columns[i], owner->registry);
    detail::check_row(table, row, owner->registry, detail::checks(table, owner->registry));
}
} // namespace coresql
