#include "ast.hpp"
#include <set>
namespace coresql::sql::detail {
std::vector<std::pair<std::string, Node>> resolve_using(Select& select, const Schema& schema,
                                                        std::map<std::string, Node>& merged) {
    if (std::none_of(select.sources.begin(), select.sources.end(),
                     [](const Source& source) { return !source.using_columns.empty(); }))
        return {};
    std::vector<std::pair<std::string, Node>> visible;
    std::set<std::string> merged_names;
    for (auto& source : select.sources) {
        const auto table = schema.find(source.table);
        if (table == schema.end())
            throw Error(ErrorCode::schema, "Unknown SQL table: " + source.table);
        auto column = [&](const std::string& name) {
            Node n;
            n.kind = Node::column;
            n.name = name;
            n.qualifier = source.alias;
            return n;
        };
        std::set<std::string> keys;
        for (const auto& name : source.using_columns) {
            if (!keys.insert(name).second)
                throw Error(ErrorCode::schema, "Duplicate USING column");
            auto left = visible.end();
            for (auto it = visible.begin(); it != visible.end(); ++it)
                if (it->first == name) {
                    if (left != visible.end())
                        throw Error(ErrorCode::schema, "Ambiguous left USING column: " + name);
                    left = it;
                }
            if (left == visible.end() || std::none_of(table->second.begin(), table->second.end(),
                                                      [&](const Column& c) { return c.name == name; }))
                throw Error(ErrorCode::schema, "USING column must exist on both sides: " + name);
            auto right = column(name);
            Node equality;
            equality.kind = Node::binary;
            equality.name = "=";
            equality.args = {left->second, right};
            if (source.on) {
                Node both;
                both.kind = Node::binary;
                both.name = "and";
                both.args = {*source.on, equality};
                source.on = std::move(both);
            } else
                source.on = std::move(equality);
            if (source.kind == JoinKind::right)
                left->second = right;
            if (source.kind == JoinKind::full) {
                Node coalesced;
                coalesced.kind = Node::function;
                coalesced.name = "coalesce";
                coalesced.args = {left->second, right};
                left->second = std::move(coalesced);
            }
            merged_names.insert(name);
        }
        for (const auto& c : table->second)
            if (!keys.contains(c.name))
                visible.emplace_back(c.name, column(c.name));
        source.using_columns.clear();
        // Each ON clause sees the merged names at its own join stage. A later
        // RIGHT/FULL USING must not redirect an earlier unqualified reference.
        merged.clear();
        for (const auto& name : merged_names) {
            const auto count =
                std::count_if(visible.begin(), visible.end(), [&](const auto& c) { return c.first == name; });
            if (count == 1)
                merged.emplace(name, std::find_if(visible.begin(), visible.end(), [&](const auto& c) {
                                         return c.first == name;
                                     })->second);
        }
        source.merged_columns = merged;
    }
    return visible;
}
} // namespace coresql::sql::detail
