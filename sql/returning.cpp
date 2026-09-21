#include "ast.hpp"
namespace coresql::sql::detail {
std::vector<std::size_t> returning_columns(const Statement& s, const std::vector<Column>& columns,
                                           Result& result) {
    std::vector<std::size_t> returning;
    for (std::size_t i = 0; i < s.returning.size(); ++i) {
        const auto& n = s.returning[i];
        if (n.kind == Node::star && (n.qualifier.empty() || n.qualifier == s.table)) {
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
    return returning;
}
} // namespace coresql::sql::detail
