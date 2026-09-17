#include "sql.hpp"
#include "type.hpp"

namespace example::codes {
using namespace coresql;
sql::SqlTypeAdapter sql_adapter() {
    sql::SqlTypeAdapter adapter;
    adapter.type_id = type().id;
    adapter.names = {"code"};
    adapter.declaration = [](const sql::SqlDeclaration& d) {
        if (!d.parameters.empty())
            throw Error(ErrorCode::type, "CODE has no parameters");
        return type();
    };
    adapter.install = install;
    adapter.can_convert = [](const Type& from, const Type& to) {
        return (from == text() && to == type()) || (from == type() && to == text());
    };
    adapter.convert = [](const Value& input, const Type& to, bool) -> Value {
        return to == text() ? Value(string(input)) : value(std::get<std::string>(input));
    };
    adapter.typed_literal = value;
    adapter.operation = [](std::string_view name,
                           std::span<const Type> inputs) -> std::optional<sql::SqlOperation> {
        if (name == "code_first_letter" && inputs.size() == 1 &&
            (inputs[0] == type() || inputs[0].id.empty()))
            return sql::SqlOperation{"code.first_letter", {}, type()};
        return {};
    };
    return adapter;
}
} // namespace example::codes
