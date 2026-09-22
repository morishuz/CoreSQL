#include "ast.hpp"
#include "coresql/sql.hpp"

namespace coresql::sql {
Cursor::Cursor(QueryCursor cursor, std::vector<std::string> columns)
    : cursor_(std::move(cursor)), columns_(std::move(columns)) {
}
std::optional<Row> Cursor::next() {
    try {
        auto row = cursor_.next();
        if (row)
            for (auto& value : *row)
                value = detail::unpack(value);
        return row;
    } catch (...) {
        close();
        throw;
    }
}
Result Cursor::fetch(std::size_t max_rows) {
    Result result;
    result.columns = columns_;
    try {
        result.rows = cursor_.fetch(max_rows).rows;
        for (auto& row : result.rows)
            for (auto& value : row)
                value = detail::unpack(value);
        return result;
    } catch (...) {
        close();
        throw;
    }
}
Cursor Connection::cursor(const Statement& statement, std::span<const Value> parameters,
                          const QueryOptions& options) {
    if (!statement.parsed_)
        throw Error(ErrorCode::state, "SQL statement was moved from");
    if (!adapters_.same_configuration(statement.adapters_))
        throw Error(ErrorCode::type, "SQL statement and connection use different type adapters");
    auto temporary = transaction_ ? std::optional<Transaction>{} : std::optional{database_.begin()};
    auto& tx = transaction_ ? *transaction_ : *temporary;
    std::vector<std::string> names;
    auto query = prepare_query(tx, statement, parameters, names);
    return Cursor(tx.cursor(query, options), std::move(names));
}
} // namespace coresql::sql
