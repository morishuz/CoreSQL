#include "ast.hpp"
#include <set>

namespace coresql::sql::detail {
Result execute(Transaction& tx, const Statement& s, const Registry& registry,
               std::span<const Value> parameters, const TypeAdapters& adapters) {
    auto schema = tx.schema();
    Lowerer lower{schema, registry, parameters, {}};
    lower.types = &adapters;
    auto select = [&](const Select& query) -> coresql::Result { return tx.query(lower.query(query)); };
    if (s.kind == Statement::select) {
        std::vector<Column> output;
        auto query = lower.query(*s.query, &output);
        Result result{tx.query(query).rows};
        for (const auto& column : output)
            result.columns.push_back(column.name);
        return result;
    }
    if (s.kind == Statement::integrity) {
        tx.integrity_check();
        return {{{std::string("ok")}}};
    }
    if (s.kind == Statement::analyze) {
        (void)tx.analyze();
        return {};
    }
    // Core mutations already have the strong guarantee. Only SQL statements
    // that combine several mutations need an additional savepoint.
    std::optional<Transaction::Savepoint> scope;
    if (s.kind == Statement::create_table)
        scope.emplace(tx.savepoint());
    Result result;
    auto field = [&](const Field& f) {
        Column c{f.name, f.type, f.primary};
        c.nullable = f.nullable && !f.primary;
        if (f.value) {
            auto v = evaluate_constant(lower.stored_expression(*f.value, f.type), registry);
            c.default_value = convert(std::move(v), f.type, false, adapters);
        }
        if (c.nullable && !c.default_value)
            c.default_value = Null(c.type);
        return c;
    };
    switch (s.kind) {
    case Statement::create_table: {
        if (s.if_not_exists && schema.contains(s.table))
            break;
        std::vector<Column> columns;
        for (const auto& f : s.fields)
            columns.push_back(field(f));
        tx.create_table(s.table, std::move(columns));
        for (const auto& f : s.fields)
            if (f.unique && !f.primary)
                tx.create_index(s.table, {"sql.unique." + f.name, {f.name}, true});
        break;
    }
    case Statement::create_index: {
        if (s.index.name.starts_with("sql.unique."))
            throw Error(ErrorCode::schema, "Reserved constraint index name");
        for (const auto& [name, columns] : schema) {
            (void)columns;
            for (const auto& index : tx.indexes(name))
                if (index.name == s.index.name) {
                    if (s.if_not_exists)
                        return {};
                    throw Error(ErrorCode::schema, "Index already exists");
                }
        }
        tx.create_index(s.table, s.index);
        break;
    }
    case Statement::drop_table:
        if (!s.if_exists || schema.contains(s.table))
            tx.drop_table(s.table);
        break;
    case Statement::drop_index: {
        std::vector<std::string> matches;
        for (const auto& [name, columns] : schema) {
            (void)columns;
            for (const auto& index : tx.indexes(name))
                if (index.name == s.table)
                    matches.push_back(name);
        }
        if (matches.size() > 1)
            throw Error(ErrorCode::schema, "Ambiguous legacy index name");
        if (matches.empty()) {
            if (!s.if_exists)
                throw Error(ErrorCode::schema, "Unknown index: " + s.table);
        } else
            tx.drop_index(matches.front(), s.table);
        break;
    }
    case Statement::rename_table:
        tx.rename_table(s.table, s.replacement);
        break;
    case Statement::rename_column:
        tx.rename_column(s.table, s.old_column, s.replacement);
        break;
    case Statement::add_column:
        if (s.fields[0].unique)
            throw Error(ErrorCode::unsupported, "ADD COLUMN UNIQUE is unsupported");
        tx.add_column(s.table, field(s.fields[0]));
        break;
    case Statement::vacuum:
        tx.vacuum();
        break;
    case Statement::insert:
        result = insert(tx, s, registry, parameters, adapters);
        break;
    case Statement::update:
    case Statement::erase: {
        lower.sources = {{s.table, s.table}};
        std::optional<Predicate> where;
        if (s.where)
            where = lower.predicate(*s.where);
        if (s.kind == Statement::erase)
            result.changes = tx.erase(s.table, std::move(where));
        else {
            std::vector<Assignment> assignments;
            for (const auto& [name, n] : s.assignments) {
                const auto target = schema.find(s.table);
                if (target == schema.end())
                    throw Error(ErrorCode::schema, "Unknown SQL table: " + s.table);
                const auto& columns = target->second;
                auto column = std::find_if(columns.begin(), columns.end(),
                                           [&](const Column& c) { return c.name == name; });
                if (column == columns.end())
                    throw Error(ErrorCode::schema, "Unknown column: " + name);
                auto e = lower.stored_expression(n, column->type);
                if (e.kind == Expr::Kind::literal && is_null(e.value))
                    e.value = Null(column->type);
                auto source = lower.expression_type(e);
                if (source && *source != column->type && convertible(*source, column->type, adapters)) {
                    e = lower.conversion(std::move(e), column->type);
                }
                assignments.push_back({name, std::move(e)});
            }
            result.changes = tx.update(s.table, std::move(assignments), std::move(where));
        }
        break;
    }
    default:
        throw Error(ErrorCode::state, "Unexpected transactional SQL statement");
    }
    if (scope)
        scope->release();
    return result;
}
} // namespace coresql::sql::detail
namespace coresql::sql {
Connection::Connection(Database& db, const Registry& registry, const TypeAdapters& adapters)
    : database_(db), registry_(registry), adapters_(adapters) {
}
Result Connection::execute(std::string_view text, std::span<const Value> parameters) {
    return execute(Statement(text, adapters_), parameters);
}
Result Connection::execute(const Statement& statement, std::span<const Value> parameters) {
    if (!statement.parsed_)
        throw Error(ErrorCode::state, "SQL statement was moved from");
    if (!adapters_.same_configuration(statement.adapters_))
        throw Error(ErrorCode::type, "SQL statement and connection use different type adapters");
    const auto& s = *statement.parsed_;
    if (parameters.size() != s.parameters)
        throw Error(ErrorCode::type, "SQL parameter count differs");
    if (s.kind == detail::Statement::savepoint) {
        bool started = !transaction_;
        if (started)
            transaction_.emplace(database_.begin());
        try {
            savepoints_.push_back({s.table, transaction_->savepoint()});
        } catch (...) {
            if (started)
                transaction_.reset();
            throw;
        }
        if (started)
            savepoint_transaction_ = true;
        return {};
    }
    if (s.kind == detail::Statement::release || s.kind == detail::Statement::rollback_to) {
        auto it = std::find_if(savepoints_.rbegin(), savepoints_.rend(),
                               [&](const NamedSavepoint& p) { return p.name == s.table; });
        if (it == savepoints_.rend())
            throw Error(ErrorCode::state, "Unknown savepoint: " + s.table);
        auto position = static_cast<std::size_t>(savepoints_.rend() - it - 1);
        if (s.kind == detail::Statement::rollback_to) {
            savepoints_[position].guard.rollback_to();
            savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(position + 1),
                              savepoints_.end());
        } else {
            savepoints_[position].guard.release();
            savepoints_.erase(savepoints_.begin() + static_cast<std::ptrdiff_t>(position), savepoints_.end());
            if (savepoints_.empty() && savepoint_transaction_) {
                transaction_->commit();
                transaction_.reset();
                savepoint_transaction_ = false;
            }
        }
        return {};
    }
    if (s.kind == detail::Statement::begin) {
        if (transaction_)
            throw Error(ErrorCode::state, "SQL transaction already active");
        transaction_.emplace(database_.begin());
        return {};
    }
    if (s.kind == detail::Statement::commit) {
        if (!transaction_)
            throw Error(ErrorCode::state, "No SQL transaction");
        if (!savepoints_.empty())
            savepoints_.front().guard.release();
        savepoints_.clear();
        transaction_->commit();
        transaction_.reset();
        savepoint_transaction_ = false;
        return {};
    }
    if (s.kind == detail::Statement::rollback) {
        if (!transaction_)
            throw Error(ErrorCode::state, "No SQL transaction");
        transaction_.reset();
        savepoints_.clear();
        savepoint_transaction_ = false;
        return {};
    }
    if (s.kind == detail::Statement::vacuum && transaction_)
        throw Error(ErrorCode::state, "VACUUM inside transaction is unsupported");
    Result result;
    if (transaction_)
        result = detail::execute(*transaction_, s, registry_, parameters, adapters_);
    else {
        auto tx = database_.begin();
        result = detail::execute(tx, s, registry_, parameters, adapters_);
        tx.commit();
    }
    for (auto& row : result.rows)
        for (auto& v : row)
            v = detail::unpack(v);
    return result;
}
} // namespace coresql::sql
