#include "ast.hpp"
namespace coresql::sql {
ReadConnection::ReadConnection(ReadSnapshot snapshot, const TypeAdapters& adapters)
    : snapshot_(std::move(snapshot)), adapters_(adapters) {
}
Query ReadConnection::prepare(const Statement& statement, std::span<const Value> parameters,
                              std::vector<std::string>& names) const {
    if (!statement.parsed_)
        throw Error(ErrorCode::state, "SQL statement was moved from");
    if (!adapters_.same_configuration(statement.adapters_))
        throw Error(ErrorCode::type, "SQL statement and connection use different type adapters");
    const auto& parsed = *statement.parsed_;
    if (parsed.kind != detail::Statement::select)
        throw Error(ErrorCode::unsupported, "Read connections require SELECT");
    if (parameters.size() != parsed.parameters)
        throw Error(ErrorCode::type, "SQL parameter count differs");
    const auto schema = snapshot_.schema();
    detail::Lowerer lower{schema, snapshot_.registry(), parameters, {}};
    lower.types = &adapters_;
    std::vector<Column> columns;
    auto query = lower.query(*parsed.query, &columns);
    for (const auto& column : columns)
        names.push_back(column.name);
    return query;
}
Result ReadConnection::query(const Statement& statement, std::span<const Value> parameters,
                             const QueryOptions& options) const {
    Result result;
    auto query = prepare(statement, parameters, result.columns);
    result.rows = snapshot_.query(query, options).rows;
    for (auto& row : result.rows)
        for (auto& value : row)
            value = detail::unpack(value);
    return result;
}
Cursor ReadConnection::cursor(const Statement& statement, std::span<const Value> parameters,
                              const QueryOptions& options) const {
    std::vector<std::string> names;
    auto query = prepare(statement, parameters, names);
    return Cursor(snapshot_.cursor(query, options), std::move(names));
}
std::vector<std::string> ReadConnection::query_each(const Statement& statement, const RowVisitor& visitor,
                                                    std::span<const Value> parameters,
                                                    const QueryOptions& options) const {
    if (!visitor)
        throw Error(ErrorCode::state, "Streaming requires a row visitor");
    std::vector<std::string> names;
    auto query = prepare(statement, parameters, names);
    snapshot_.query_each(
        query,
        [&](std::span<const Value> row) {
            Row unpacked;
            unpacked.reserve(row.size());
            for (const auto& value : row)
                unpacked.push_back(detail::unpack(value));
            return visitor(unpacked);
        },
        options);
    return names;
}
} // namespace coresql::sql
