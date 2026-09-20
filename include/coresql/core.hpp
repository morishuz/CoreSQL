#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace coresql {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;
enum class ErrorCode { schema, type, unsupported, conflict, state, format, io, constraint };
class Error : public std::runtime_error {
public:
    ErrorCode code;
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code(code) {}
};

// Stable names + versions are persistent identity; parameters are extension-owned.
struct Type {
    std::string id;
    std::uint32_t version = 1;
    Bytes parameters;
    bool operator==(const Type&) const = default;
};
Type integer();
Type real();
Type text();

class Opaque {
public:
    Opaque(Type type, Bytes bytes);
    const Type& type() const { return data_->type; }
    ByteView bytes() const { return data_->bytes; }
    bool operator==(const Opaque& b) const {
        return type() == b.type() && std::ranges::equal(bytes(), b.bytes());
    }

private:
    struct Data {
        Type type;
        Bytes bytes;
    };
    std::shared_ptr<const Data> data_;
};
// Typed NULL; stored columns accept it only when Column::nullable is true.
struct Null {
    std::shared_ptr<const Type> type;
    bool operator==(const Null& b) const { return type && b.type && *type == *b.type; }
    explicit Null(Type t) : type(std::make_shared<const Type>(std::move(t))) {}
};
// Tagged compact cell. Default identities stay untagged; custom i64/i128 use this.
class Compact {
public:
    Compact(const Type& type, std::int64_t payload);
    Compact(const Type& type, std::array<std::byte, 16> payload);
    const Type& type() const { return *reinterpret_cast<const Type*>(bits_ & ~std::uintptr_t{1}); }
    ByteView bytes() const { return {payload_.data(), (bits_ & 1) ? std::size_t{16} : std::size_t{8}}; }
    bool operator==(const Compact& b) const {
        return type() == b.type() && std::ranges::equal(bytes(), b.bytes());
    }

private:
    void bind(const Type* type, std::uint8_t width) {
        bits_ = reinterpret_cast<std::uintptr_t>(type) | std::uintptr_t{width == 16};
    }
    std::uintptr_t bits_ = 0;
    std::array<std::byte, 16> payload_{};
};
using Value = std::variant<std::int64_t, double, std::string, Opaque, Null, Compact>;
Value compact(const Type&, std::int64_t);
Value compact(const Type&, std::array<std::byte, 16>);
// Interns a type identity for compact cells. Returned references are process-lifetime.
const Type& intern(const Type&);
inline bool is_null(const Value& v) {
    return std::holds_alternative<Null>(v);
}
Type type_of(const Value&);

// Callbacks are trusted, deterministic, side-effect-free application code.
// Views are borrowed for the duration of a call. Throw Error for invalid input.
struct EncodedTypeAddon {
    std::string id;
    std::uint32_t version = 1;
    std::function<void(ByteView parameters)> validate_type;
    std::function<void(ByteView parameters, ByteView value)> validate_value;
    std::function<bool(ByteView parameters, ByteView a, ByteView b)> equal;
    std::function<int(ByteView parameters, ByteView a, ByteView b)> compare;
};
// Closed cell layouts. Identity is open; a type chooses one of these payloads.
// i64/f64/text have default identities (integer/real/text) and may be untagged.
enum class Layout { i64, f64, text, bytes, i128 };
struct TypeAddon {
    std::string id;
    std::uint32_t version = 1;
    Layout layout = Layout::bytes;
    std::function<void(ByteView)> validate_type;
    std::function<void(ByteView, const Value&)> validate_value;
    std::function<bool(ByteView, const Value&, const Value&)> equal;
    std::function<int(ByteView, const Value&, const Value&)> compare;
    // Equal values must hash identically, including alternative encodings.
    std::function<std::size_t(ByteView, const Value&)> hash;
    // Opt-in: equal/compare/hash are the layout's native C operators.
    // Providers customizing those semantics must leave this false.
    bool native_ops = false;
};
inline bool native_i64(const TypeAddon& addon) {
    return addon.layout == Layout::i64 && addon.native_ops;
}
inline bool native_scalar(const TypeAddon& addon) {
    return addon.native_ops && addon.layout != Layout::bytes;
}
// Payload of an i64 cell: untagged INTEGER or a tagged Compact i64.
std::int64_t i64_payload(const Value&);
std::array<std::byte, 16> i128_payload(const Value&);
TypeAddon encoded_type(EncodedTypeAddon);
void install_scalar_types(class Registry&);

// Internal, snapshot-local locations; not persistent application row identities.
struct RowLocation {
    std::uint64_t chunk;
    std::uint32_t slot;
    auto operator<=>(const RowLocation&) const = default;
};
struct IndexResult {
    std::vector<RowLocation> rows;
    // True certifies the requested predicate for every returned row. False
    // requires an exact recheck. Neither mode may omit matching rows.
    bool exact = false;
};

struct IndexEntry {
    Value value;
    RowLocation location;
};

// Snapshot-local index. clone() must isolate subsequent mutations (COW is fine).
// insert() has the strong exception guarantee; erase() may throw. The engine
// stages edits and alone controls publication, rollback and durable commits.
class Index {
public:
    virtual ~Index() = default;
    virtual std::shared_ptr<Index> clone() const = 0;
    virtual void insert(const Value&, RowLocation) = 0;
    virtual void erase(const Value&, RowLocation) = 0;
    // Verify all entries against source rows, including omissions and extras.
    // Throw state on mismatch; default throws unsupported. Must not mutate.
    virtual void validate(std::span<const IndexEntry>) const;
    // Unique lookup certifies equality, with no false positives or omissions.
    virtual std::optional<RowLocation> lookup(const Value&) const;
    virtual void validate_search(const std::string& operation) const;
    virtual Type search_type(const std::string& operation, const Type& source) const;
    virtual IndexResult search(const std::string&, const Value&) const;
    virtual bool matches(const std::string&, const Value&, const Value&) const;
};
struct IndexAddon {
    std::string id;
    std::function<std::shared_ptr<Index>(const Type&, const TypeAddon&)> create;
    bool unique = false;
};
std::shared_ptr<Index> make_hash_index(const Type&, const TypeAddon&);

// A named, immutable projection from one value to zero or more equality keys.
// Names identify the complete extraction semantics (including configuration).
// Change the name/version when changing those semantics in a persistent schema.
struct KeyExtractor {
    std::string name;
    Type source_type, key_type;
    std::function<std::vector<Value>(const Value&)> extract;
};

struct Function {
    std::string name;
    std::function<Type(std::span<const Type>)> infer;
    std::function<Value(std::span<const Value>)> invoke;
    bool accepts_null = false;
    // Optional type-only specialization, called after inference/validation.
    // The returned callback must own anything retained from these borrowed types
    // and preserve invoke semantics. NULL handling and result validation stay in core.
    std::function<std::function<Value(std::span<const Value>)>(std::span<const Type>, const Type&)> prepare =
        {};
    // Optional exact membership specialization for one bound type. Candidates
    // are validated, non-NULL and borrowed. An empty callback requests fallback.
    // The returned predicate owns its state and must be total and side-effect-free.
    std::function<std::function<bool(const Value&)>(const Type&, std::span<const Value>)> prepare_membership =
        {};
};

class AggregateState {
public:
    virtual ~AggregateState() = default;
    virtual void step(std::span<const Value>) = 0;
    virtual Value finish() = 0;
};
struct AggregateFunction {
    std::string name;
    std::function<Type(std::span<const Type>)> infer;
    std::function<std::unique_ptr<AggregateState>(std::span<const Type>, const Registry&)> create;
};
void install_aggregate_functions(class Registry&);
void install_relational_functions(class Registry&);
namespace detail {
struct Comparison;
}

class Registry {
public:
    Registry(bool scalar_types = true);
    void add(TypeAddon);
    void add(EncodedTypeAddon type) { add(encoded_type(std::move(type))); }
    void add(IndexAddon);
    // Also registers a shared non-unique postings index under the same name.
    void add(KeyExtractor);
    const KeyExtractor& extractor(const std::string&) const;
    const TypeAddon& addon(const Type&) const;
    std::shared_ptr<Index> index(const std::string&, const Type&, bool require_unique = false) const;
    void add(Function);
    void add(AggregateFunction);
    const AggregateFunction& aggregate(const std::string&) const;
    void validate(const Type&) const;
    void validate(const Value&, const Type& expected) const;
    bool orderable(const Type&) const;
    bool equatable(const Type&) const;
    int compare(const Value&, const Value&) const;
    bool equal(const Value&, const Value&) const;
    const Function& function(const std::string&) const;

private:
    friend struct detail::Comparison;
    std::map<std::pair<std::string, std::uint32_t>, TypeAddon> types_;
    std::map<std::string, Function> functions_;
    std::map<std::string, AggregateFunction> aggregates_;
    std::map<std::string, IndexAddon> indexes_;
    std::map<std::string, KeyExtractor> extractors_;
};

struct Query;
// Logical queries: no SQL text, storage handles, or physical plans in this API.
struct Expr {
    enum class Kind {
        column,
        literal,
        call,
        aggregate,
        row_id,
        parameter,
        subquery,
        conditional,
        exists,
        coalesce,
        membership
    } kind;
    std::string name;
    Value value = std::int64_t{0};
    std::vector<Expr> arguments;
    std::string qualifier = {};
    std::shared_ptr<const Query> subquery = {};
    bool distinct = false;
    std::vector<std::string> parameters = {};
};
Expr column(std::string name);
Expr column(std::string qualifier, std::string name);
Expr literal(Value value);
Expr call(std::string name, std::vector<Expr> arguments);
Expr aggregate(std::string name, std::vector<Expr> arguments = {}, bool distinct = false);
// Lazy branches: every arm binds, but only the selected result executes.
// A matching form evaluates its base once and uses a registered equality function.
Expr choose(std::vector<std::pair<Expr, Expr>> branches, std::optional<Expr> otherwise = {});
Expr choose(Expr base, std::string equality, std::vector<std::pair<Expr, Expr>> branches,
            std::optional<Expr> otherwise = {});
Expr coalesce(std::vector<Expr>);
Value evaluate_constant(const Expr&, const Registry&);
Expr row_id();
Expr parameter(std::string name);
Expr exists(Query, std::vector<std::pair<std::string, Expr>> bindings = {});
Expr membership(Expr value, std::vector<Expr> candidates, std::string equality_function);
Expr membership(Expr value, Query candidates, std::string equality_function,
                std::vector<std::pair<std::string, Expr>> bindings = {});
// Returns the first result row, or typed NULL if empty; use ORDER BY to choose it.
Expr scalar_subquery(Query, std::vector<std::pair<std::string, Expr>> bindings = {});
enum class Compare { equal, less, greater, not_equal, less_equal, greater_equal };
struct Predicate {
    Expr left{Expr::Kind::literal, {}, std::int64_t{0}, {}};
    Compare operation = Compare::equal;
    Expr right{Expr::Kind::literal, {}, std::int64_t{0}, {}};
    enum class Kind { comparison, all, any, negation, contains } kind = Kind::comparison;
    std::vector<Predicate> children = {};
    std::string extractor = {};
};
// Ordered, short-circuiting conditions. Empty all_of is true; empty any_of false.
Predicate all_of(std::vector<Predicate>);
Predicate any_of(std::vector<Predicate>);
Predicate not_(Predicate);
// Membership is independent of whether the source column has this index.
Predicate contains(Expr source, std::string extractor, Expr key);
struct Assignment {
    std::string column;
    Expr value;
};
struct Order {
    Expr expression;
    bool descending = false;
};
// ON supplies a general join condition; otherwise cross selects a Cartesian
// product or left/right supply qualified equality columns, one per side.
// right_where filters unqualified right columns before matching.
enum class JoinKind { inner, left, right, full };
struct Join {
    std::string table, alias;
    Expr left = literal(std::int64_t{0}), right = literal(std::int64_t{0});
    bool cross = false;
    std::optional<Predicate> right_where = {};
    JoinKind kind = JoinKind::inner;
    std::optional<Predicate> on = {};
};
using InnerJoin = Join; // Source compatibility with the original equality-join API.
struct IndexSearch {
    std::string column;
    std::string operation;
    Value value;
};
enum class SetOperation { union_all, union_distinct, intersect, except };
struct Column {
    std::string name;
    Type type;
    bool primary_key = false;
    std::string index = {};
    std::optional<Value> default_value = {};
    bool nullable = false;
    bool operator==(const Column&) const = default;
};
// Named, query-local materialization. Only output column names/types are used;
// schema constraints and indexes are not inherited. Referenced definitions are
// materialized once in dependency/declaration order; LIMIT 0 binds shapes only.
struct Relation {
    std::string name;
    std::shared_ptr<const Query> query;
    std::vector<Column> columns;
};
struct Query {
    std::string table;
    std::vector<Expr> select; // Empty projection means all columns.
    std::optional<Predicate> where;
    std::vector<Order> order_by;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    std::string alias = {};
    std::optional<Join> join = {};
    std::optional<IndexSearch> search = {};
    bool distinct = false;
    std::vector<Join> joins = {};
    std::optional<Predicate> source_where = {};
    std::vector<std::pair<SetOperation, std::shared_ptr<const Query>>> compounds = {};
    std::vector<Expr> group_by = {};
    std::optional<Predicate> having = {};
    // Opt-in: evaluation is side-effect-free and repeatable within one snapshot.
    bool repeatable = false;
    std::vector<Relation> relations = {};
    std::size_t offset = 0;
};
struct IndexDefinition {
    std::string name;
    std::vector<std::string> columns;
    bool unique = false;
    std::vector<bool> descending = {};
    bool operator==(const IndexDefinition&) const = default;
};
using Schema = std::map<std::string, std::vector<Column>>;
using Row = std::vector<Value>;
struct Result {
    std::vector<Type> types;
    std::vector<Row> rows;
};
struct Stats {
    std::size_t tables = 0;
    std::size_t rows = 0;
    std::size_t stored_payload_bytes = 0;
    std::size_t peak_stored_payload_bytes = 0;
};

struct StorageStats {
    // Bytes submitted by this handle, including checkpoints; not device writes.
    std::uint64_t bytes_written = 0, checkpoints = 0;
};

namespace detail {
struct State;
struct Owner;
} // namespace detail

class Transaction;
// All calls on a database and its transactions require external serialization,
// including queries concurrent with writes. Shared snapshots do not imply thread safety.
class Database {
public:
    explicit Database(Registry registry = {});
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;
    // Persistent mode: exclusive local-file ownership; commits synchronize the log.
    static Database open(const std::filesystem::path&, Registry registry = {});
    bool persistent() const;
    // Synchronized native backup to a new file; includes committed state only.
    void backup(const std::filesystem::path&) const;
    // Import a snapshot export into a new persistent file, preserving identities/indexes.
    static Database restore(const std::filesystem::path& snapshot, const std::filesystem::path& destination,
                            Registry registry = {});
    static std::size_t encoded_size_limit();
    // Reclaim obsolete persistent records; no effect on transaction snapshots.
    void checkpoint();
    StorageStats storage_stats() const;
    Transaction begin();
    Result query(const Query&) const;
    Stats stats() const;
    // Owned schema copy; safe across commits and database destruction.
    Schema schema() const;
    // Explicit exports, NOT durable transaction commits. Path must not exist.
    void save(const std::filesystem::path&) const;
    static Database load(const std::filesystem::path&, Registry registry = {});

private:
    friend class Transaction;
    static Database decode(ByteView, Registry);
    std::shared_ptr<detail::Owner> owner_;
};

class Transaction {
    struct SavepointState;

public:
    // Scoped rollback. Release keeps changes; neither operation commits them.
    // Closing an outer scope closes its descendants. Closed guards are inert.
    class Savepoint {
    public:
        Savepoint(const Savepoint&) = delete;
        Savepoint& operator=(const Savepoint&) = delete;
        Savepoint(Savepoint&&) noexcept;
        Savepoint& operator=(Savepoint&&) noexcept;
        ~Savepoint();
        void release() noexcept;
        void rollback() noexcept;
        // Restore this point and keep it active; closes descendants.
        void rollback_to();

    private:
        friend class Transaction;
        explicit Savepoint(std::shared_ptr<SavepointState>);
        std::shared_ptr<SavepointState> state_;
    };
    // Commit requires all scopes closed; whole-transaction rollback closes them.
    [[nodiscard]] Savepoint savepoint();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();
    Transaction(Transaction&&) noexcept;
    Transaction& operator=(Transaction&&) noexcept;
    void create_table(std::string name, std::vector<Column> columns);
    void insert(const std::string& table, Row row);
    void create_index(const std::string& table, IndexDefinition);
    std::vector<IndexDefinition> indexes(const std::string& table) const;
    void drop_table(const std::string& table);
    void drop_index(const std::string& table, const std::string& index);
    void rename_table(const std::string& table, const std::string& replacement);
    void rename_column(const std::string& table, const std::string& column, const std::string& replacement);
    void add_column(const std::string& table, Column);
    void insert_from(const std::string& table, const Query&, bool replace = false);
    void replace(const std::string& table, Row);
    void vacuum();
    void integrity_check() const;
    std::map<std::string, std::size_t> analyze() const;
    std::size_t update(const std::string& table, std::vector<Assignment>,
                       std::optional<Predicate> where = {});
    std::size_t erase(const std::string& table, std::optional<Predicate> where = {});
    Result query(const Query&) const;
    Schema schema() const;
    void commit();
    void rollback() noexcept;
    // Destruction discards staged state. Calls require external serialization.
private:
    friend class Database;
    explicit Transaction(const std::shared_ptr<detail::Owner>&);
    std::weak_ptr<detail::Owner> owner_;
    std::shared_ptr<const detail::State> base_;
    std::unique_ptr<detail::State> staged_;
    bool dirty_ = false;
    std::shared_ptr<SavepointState> savepoint_;
    void finish_savepoint(SavepointState*, bool restore) noexcept;
    void retarget_savepoints() noexcept;
    std::shared_ptr<detail::Owner> active() const;
    std::size_t erase_impl(const std::string&, std::optional<Predicate>, std::optional<IndexResult>);
    // Snapshot import supplies logical IDs without depending on insertion layout.
    void insert_impl(const std::string&, Row, std::optional<std::int64_t>);
    void restore_row_sequence(const std::string&, std::int64_t);
};

} // namespace coresql
