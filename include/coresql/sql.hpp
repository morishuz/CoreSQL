#pragma once
#include "coresql/core.hpp"
#include "coresql/sql_types.hpp"
#include <string_view>

namespace coresql::sql {
// Install SQL semantics and registered type adapters before constructing the database.
// Defaults include DATE/DECIMAL/VECTOR/BLOB. Call once; do not separately install their add-ons.
void install(Registry&, const TypeAdapters& = default_type_adapters());
namespace detail {
struct Statement;
}
class Statement {
public:
    explicit Statement(std::string_view sql, const TypeAdapters& = default_type_adapters());
    std::size_t parameter_count() const;
    // One-based slot for the exact, case-sensitive name including :/@/$ prefix.
    // Repeated names share a slot. Empty means the statement has no such name.
    std::optional<std::size_t> parameter_index(std::string_view name) const;

private:
    friend class Connection;
    friend class ReadConnection;
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
class Cursor {
public:
    Cursor(QueryCursor, std::vector<std::string> columns);
    Cursor(Cursor&&) noexcept = default;
    Cursor& operator=(Cursor&&) noexcept = default;
    const std::vector<std::string>& columns() const { return columns_; }
    std::optional<Row> next();
    Result fetch(std::size_t max_rows);
    CursorStats stats() const { return cursor_.stats(); }
    void close() noexcept { cursor_.close(); }

private:
    QueryCursor cursor_;
    std::vector<std::string> columns_;
};
// Read-only SQL over a retained committed snapshot. Distinct connections may run
// on separate reader threads; a cursor retains the same snapshot independently.
class ReadConnection {
public:
    explicit ReadConnection(ReadSnapshot, const TypeAdapters& = default_type_adapters());
    Result query(const Statement&, std::span<const Value> = {}, const QueryOptions& = {}) const;
    Cursor cursor(const Statement&, std::span<const Value> = {}, const QueryOptions& = {}) const;
    std::vector<std::string> query_each(const Statement&, const RowVisitor&, std::span<const Value> = {},
                                        const QueryOptions& = {}) const;

private:
    ReadSnapshot snapshot_;
    TypeAdapters adapters_;
    Query prepare(const Statement&, std::span<const Value>, std::vector<std::string>&) const;
};
class Connection {
public:
    struct QueryCacheStats {
        std::size_t hits = 0, misses = 0;
    };
    // Database must outlive this connection and not move. Supply the same registry
    // used to construct it; both keep immutable copies of their type contracts.
    Connection(Database&, const Registry&, const TypeAdapters& = default_type_adapters());
    Result execute(const Statement&, std::span<const Value> parameters = {});
    Result execute(std::string_view, std::span<const Value> parameters = {});
    // Controlled SELECT execution; mutations/transaction commands are rejected.
    Result query(const Statement&, std::span<const Value> parameters = {}, const QueryOptions& = {});
    Cursor cursor(const Statement&, std::span<const Value> parameters = {}, const QueryOptions& = {});
    // Streaming SELECT follows the core cursor contract, including UNION ALL and one join.
    // Returns output column names. Each unpacked row is borrowed during the visitor.
    std::vector<std::string> query_each(const Statement&, const RowVisitor&,
                                        std::span<const Value> parameters = {}, const QueryOptions& = {});
    bool in_transaction() const { return transaction_.has_value(); }
    QueryCacheStats query_cache_stats() const { return query_cache_stats_; }
    void clear_query_cache() { query_cache_.reset(); }
    // Opt in to a typed, parameterized logical SELECT plan; bindings remain snapshot-local.
    void enable_query_cache(bool enabled) {
        query_cache_enabled_ = enabled;
        clear_query_cache();
    }

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
    struct CachedQuery {
        std::shared_ptr<const detail::Statement> statement;
        Schema schema;
        std::vector<std::pair<Type, bool>> parameters;
        Query query;
        std::vector<std::string> names;
    };
    std::optional<CachedQuery> query_cache_;
    QueryCacheStats query_cache_stats_;
    bool query_cache_enabled_ = false;
    Query prepare_query(Transaction&, const Statement&, std::span<const Value>, std::vector<std::string>&);
};
} // namespace coresql::sql
