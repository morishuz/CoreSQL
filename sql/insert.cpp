#include "ast.hpp"
#include <set>

namespace coresql::sql::detail {
Result insert(Transaction& tx, const Statement& s, const Registry& registry,
              std::span<const Value> parameters, const TypeAdapters& adapters) {
    auto schema = tx.schema();
    Lowerer lower{schema, registry, parameters, {}};
    lower.types = &adapters;
    auto select = [&](const Select& query) { return tx.query(lower.query(query)); };
    Result result;
    // RETURNING allocations and all rows form one atomic SQL statement.
    std::optional<Transaction::Savepoint> scope;
    if (s.query || s.rows.size() > 1 || !s.returning.empty())
        scope.emplace(tx.savepoint());
    auto it = schema.find(s.table);
    if (it == schema.end())
        throw Error(ErrorCode::schema, "Unknown insertion table");
    const auto& columns = it->second;
    std::optional<std::size_t> generated;
    for (std::size_t i = 0; i < columns.size(); ++i)
        if (columns[i].primary_key && columns[i].type == integer())
            generated = i;
    std::vector<std::size_t> returning;
    for (std::size_t i = 0; i < s.returning.size(); ++i) {
        const auto& n = s.returning[i];
        if (n.kind == Node::star && n.qualifier.empty()) {
            if (!s.returning_aliases[i].empty())
                throw Error(ErrorCode::schema, "RETURNING * cannot have an alias");
            for (std::size_t c = 0; c < columns.size(); ++c) {
                returning.push_back(c);
                result.columns.push_back(columns[c].name);
            }
        } else {
            if (n.kind != Node::column || (!n.qualifier.empty() && n.qualifier != s.table))
                throw Error(ErrorCode::unsupported, "RETURNING supports target columns and * only");
            auto c = std::find_if(columns.begin(), columns.end(),
                                  [&](const Column& col) { return col.name == n.name; });
            if (c == columns.end())
                throw Error(ErrorCode::schema, "Unknown RETURNING column: " + n.name);
            returning.push_back(static_cast<std::size_t>(c - columns.begin()));
            result.columns.push_back(s.returning_aliases[i].empty() ? n.name : s.returning_aliases[i]);
        }
    }
    // Compute a starting maximum lazily, once per ordinary multi-row statement.
    // REPLACE can remove the largest key through another UNIQUE constraint.
    std::optional<std::int64_t> largest;
    auto allocate = [&]() {
        if (!largest) {
            auto maximum = tx.query(Query{s.table, {aggregate("max", {column(columns[*generated].name)})}});
            largest = is_null(maximum.rows[0][0])
                          ? 0
                          : std::max(std::int64_t{0}, std::get<std::int64_t>(maximum.rows[0][0]));
        }
        if (*largest == INT64_MAX)
            throw Error(ErrorCode::constraint, "Generated integer primary key exhausted");
        return ++*largest;
    };
    std::vector<std::size_t> mapping;
    std::set<std::string> names;
    for (const auto& name : s.columns) {
        if (!names.insert(name).second)
            throw Error(ErrorCode::schema, "Duplicate insertion column");
        auto c =
            std::find_if(columns.begin(), columns.end(), [&](const Column& col) { return col.name == name; });
        if (c == columns.end())
            throw Error(ErrorCode::schema, "Unknown insertion column");
        mapping.push_back(static_cast<std::size_t>(c - columns.begin()));
    }
    auto insert = [&](Row input) {
        if (input.size() != (mapping.empty() ? columns.size() : mapping.size()))
            throw Error(ErrorCode::schema, "SQL insertion width differs");
        std::vector<std::optional<Value>> values(columns.size());
        for (std::size_t i = 0; i < input.size(); ++i)
            values[mapping.empty() ? i : mapping[i]] = std::move(input[i]);
        Row row;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (!values[i])
                values[i] = columns[i].default_value;
            if (generated == i && (!values[i] || is_null(*values[i])))
                values[i] = allocate();
            if (!values[i])
                throw Error(ErrorCode::constraint, "Missing non-NULL column: " + columns[i].name);
            row.push_back(convert(std::move(*values[i]), columns[i].type, false, adapters));
        }
        Row returned;
        for (auto c : returning)
            returned.push_back(row[c]);
        if (generated && largest)
            *largest = std::max(*largest, std::get<std::int64_t>(row[*generated]));
        if (s.replace)
            tx.replace(s.table, std::move(row));
        else
            tx.insert(s.table, std::move(row));
        if (s.replace)
            largest.reset();
        if (!s.returning.empty())
            result.rows.push_back(std::move(returned));
        ++result.changes;
    };
    if (s.default_values) {
        if (!s.columns.empty())
            throw Error(ErrorCode::schema, "DEFAULT VALUES does not accept a column list");
        Row row;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const auto& column = columns[i];
            if (!column.default_value && generated != i)
                throw Error(ErrorCode::constraint, "Missing column default: " + column.name);
            row.push_back(column.default_value.value_or(Value(Null(column.type))));
        }
        insert(std::move(row));
    } else if (s.query) {
        auto input = select(*s.query);
        if (input.types.size() != (mapping.empty() ? columns.size() : mapping.size()))
            throw Error(ErrorCode::schema, "SQL insertion projection width differs");
        for (std::size_t i = 0; i < input.types.size(); ++i) {
            const auto& target = columns[mapping.empty() ? i : mapping[i]].type;
            if (!convertible(input.types[i], target, adapters))
                throw Error(ErrorCode::type, "SQL insertion projection type differs");
        }
        std::set<std::size_t> supplied(mapping.begin(), mapping.end());
        if (!mapping.empty())
            for (std::size_t i = 0; i < columns.size(); ++i)
                if (!supplied.contains(i) && !columns[i].default_value && generated != i)
                    throw Error(ErrorCode::constraint, "Missing non-NULL insertion column");
        for (auto& row : input.rows)
            insert(std::move(row));
    } else
        for (const auto& nodes : s.rows) {
            if (nodes.size() != (mapping.empty() ? columns.size() : mapping.size()))
                throw Error(ErrorCode::schema, "SQL insertion width differs");
            Row row;
            for (std::size_t i = 0; i < nodes.size(); ++i)
                row.push_back(
                    lower.stored_constant(nodes[i], columns[mapping.empty() ? i : mapping[i]].type));
            insert(std::move(row));
        }
    if (scope)
        scope->release();
    return result;
}
} // namespace coresql::sql::detail
