#include "coresql/sql_types.hpp"
#include "coresql/date.hpp"

namespace coresql::sql {
SqlTypeAdapter date_adapter() {
    SqlTypeAdapter a;
    a.type_id = dates::type().id;
    a.names = {"date"};
    a.declaration = [](const SqlDeclaration& declaration) {
        auto p = declaration.parameters;
        if (!p.empty())
            throw Error(ErrorCode::unsupported, "DATE accepts no parameters");
        return dates::type();
    };
    a.install = dates::install;
    a.can_convert = [](const Type& s, const Type& t) {
        return (t == dates::type() && (s == text() || s.id == "sql.value")) ||
               (s == dates::type() && t == text());
    };
    a.convert = [](const Value& v, const Type& t, bool) -> Value {
        return t == text() ? Value(dates::format(v)) : dates::parse(std::get<std::string>(v));
    };
    a.typed_literal = dates::parse;
    a.fold_text_cast = true;
    a.repeatable = true;
    a.reorder_comparisons = true;
    a.operation = [](std::string_view op, std::span<const Type> types) -> std::optional<SqlOperation> {
        if (op == "sql.extract.year" && types.size() == 1 &&
            (types[0] == dates::type() || types[0].id.empty()))
            return SqlOperation{"date.year", {}, dates::type(), true};
        return {};
    };
    return a;
}
} // namespace coresql::sql
