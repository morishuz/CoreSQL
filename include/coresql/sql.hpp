#pragma once
#include "coresql/core.hpp"
#include "coresql/sql_types.hpp"
#include <string_view>

namespace coresql::sql {
// Install SQL semantics and registered type adapters before constructing the database.
// Defaults include DATE/DECIMAL/VECTOR. Call once; do not separately install their add-ons.
void install(Registry&, const TypeAdapters& = default_type_adapters());
namespace detail {
struct Statement;
}
class Statement {
public:
    explicit Statement(std::string_view sql, const TypeAdapters& = default_type_adapters());
    std::size_t parameter_count() const;

private:
    friend class Connection;
    std::shared_ptr<const detail::Statement> parsed_;
    TypeAdapters adapters_;
};
// Parse the complete script before executing any part of it.
std::vector<Statement> prepare_script(std::string_view, const TypeAdapters& = default_type_adapters());
// SQL results can have heterogeneous values in undeclared-type columns.
struct Result {
    std::vector<Row> rows;
    std::size_t changes = 0;
    std::vector<std::string> columns = {};
};
class Connection {
public:
    // Database must outlive this connection and not move. Supply the same registry
    // used to construct it; both keep immutable copies of their type contracts.
    Connection(Database&, const Registry&, const TypeAdapters& = default_type_adapters());
    Result execute(const Statement&, std::span<const Value> parameters = {});
    Result execute(std::string_view, std::span<const Value> parameters = {});
    bool in_transaction() const { return transaction_.has_value(); }

private:
    Database& database_;
    Registry registry_;
    TypeAdapters adapters_;
    std::optional<Transaction> transaction_;
    struct NamedSavepoint {
        std::string name;
        Transaction::Savepoint guard;
    };
    std::vector<NamedSavepoint> savepoints_;
    bool savepoint_transaction_ = false;
};
} // namespace coresql::sql
