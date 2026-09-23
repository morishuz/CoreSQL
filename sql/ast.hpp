#pragma once
#include "coresql/sql.hpp"
#include "value_policy.hpp"

namespace coresql::sql::detail {
struct Select;
struct Node {
    enum Kind {
        literal,
        column,
        parameter,
        function,
        binary,
        unary,
        subquery,
        star,
        case_when,
        case_match,
        exists,
        membership,
        captured_column
    } kind = literal;
    // captured_column is created only during lowering, never by the parser.
    std::string name, qualifier;
    std::string numeric_spelling; // Preserve exact numeric text for DECIMAL contexts.
    Value value = std::int64_t{0};
    std::vector<Node> args;
    std::shared_ptr<Select> query;
    std::size_t position = 0, depth = 1;
    bool distinct = false;
};
struct Source {
    std::string table, alias;
    JoinKind kind = JoinKind::inner;
    std::optional<Node> on = {};
    bool explicit_join = false;
    std::shared_ptr<Select> query = {};
    std::vector<std::string> columns = {};
    std::vector<std::string> using_columns = {};
    std::optional<std::map<std::string, Node>> merged_columns = {};
};
struct CommonTable {
    std::string name;
    std::vector<std::string> columns;
    std::shared_ptr<Select> query;
};
struct Select {
    std::vector<CommonTable> ctes;
    std::vector<Node> projection;
    std::vector<std::string> aliases;
    std::vector<Source> sources;
    std::optional<Node> where, limit, having, offset;
    std::vector<Node> group;
    std::vector<std::pair<Node, bool>> order;
    bool distinct = false;
    std::vector<std::pair<SetOperation, std::shared_ptr<Select>>> compounds;
};
struct Field {
    std::string name;
    Type type;
    bool unique = false, primary = false, nullable = true;
    std::optional<Node> value;
    std::vector<Node> checks;
    std::optional<ForeignKey> reference;
};
struct KeyConstraint {
    std::string name;
    std::vector<std::string> columns;
    bool primary = false;
};
struct Statement {
    enum Kind {
        select,
        create_table,
        create_index,
        insert,
        update,
        erase,
        add_column,
        begin,
        commit,
        rollback,
        vacuum,
        integrity,
        analyze,
        drop_table,
        drop_index,
        rename_table,
        rename_column,
        savepoint,
        release,
        rollback_to
    } kind = select;
    std::string table;
    std::vector<Field> fields;
    IndexDefinition index;
    std::vector<std::string> columns;
    std::vector<std::vector<Node>> rows;
    std::vector<std::pair<std::string, Node>> assignments;
    std::shared_ptr<Select> query;
    std::optional<Node> where;
    enum Conflict { no_conflict, do_nothing, do_update } conflict = no_conflict;
    std::vector<std::string> conflict_columns;
    bool replace = false, if_exists = false, if_not_exists = false, default_values = false;
    std::string replacement, old_column;
    std::vector<Node> returning;
    std::vector<std::string> returning_aliases;
    std::size_t parameters = 0;
    std::map<std::string, std::size_t> named_parameters;
    std::vector<KeyConstraint> keys;
    std::vector<std::pair<std::string, Node>> checks;
    std::vector<ForeignKey> foreign_keys;
};
Statement parse(std::string_view, const TypeAdapters&);
std::vector<std::pair<std::string, Node>> resolve_using(Select&, const Schema&, std::map<std::string, Node>&);
std::string function_name(const std::string&);
struct Lowerer {
    const Schema& schema;
    const Registry& registry;
    std::span<const Value> parameters;
    std::vector<Source> sources;
    const Lowerer* outer = nullptr;
    std::vector<std::pair<std::string, Expr>>* captures = nullptr;
    bool qualified = false;
    std::map<std::string, Type> parameter_types = {};
    std::map<std::string, std::string> ctes = {};
    std::shared_ptr<std::size_t> relation_id = std::make_shared<std::size_t>(0);
    const TypeAdapters* types = &default_type_adapters();
    // SQL-local template slots are substituted before crossing the core API.
    bool parameterize = false;
    std::map<std::string, Node> merged_columns = {};
    Expr expression(const Node&) const;
    std::optional<SqlOperation> operation(std::string_view, std::span<const Expr>,
                                          std::span<const Node> syntax = {}) const;
    Expr contextual_literal(const Node&, Expr, const SqlOperation&) const;
    Expr conversion(Expr, const Type&, bool explicit_cast = false) const;
    Expr stored_expression(const Node&, const Type&, bool explicit_cast = false) const;
    Value stored_constant(const Node&, const Type&) const;
    std::optional<Type> expression_type(const Expr&) const;
    void unify(std::vector<Expr*>&) const;
    Expr truth(Expr) const;
    Predicate predicate(const Node&) const;
    Query query(const Select&, std::vector<Column>* output = nullptr) const;
    Query query_body(const Select&) const;
    bool repeatable(const Select&) const;
    Value constant(const Node&) const;
};
std::vector<std::size_t> returning_columns(const Statement&, const std::vector<Column>&, Result&);
struct UpsertPlan {
    std::vector<std::vector<std::string>> keys;
};
UpsertPlan prepare_upsert(Transaction&, const Statement&, const Schema&);
bool upsert(Transaction&, const Statement&, const Registry&, std::span<const Value>, const TypeAdapters&,
            const Schema&, const UpsertPlan&, Row&);
Result insert(Transaction&, const Statement&, const Registry&, std::span<const Value>, const TypeAdapters&);
Result execute(Transaction&, const Statement&, const Registry&, std::span<const Value>, const TypeAdapters&);
} // namespace coresql::sql::detail
