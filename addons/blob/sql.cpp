#include "coresql/sql_types.hpp"
#include "coresql/blob.hpp"
namespace coresql::sql {
SqlTypeAdapter blob_adapter() {
    SqlTypeAdapter a;
    a.type_id = blobs::type().id;
    a.names = {"blob"};
    a.declaration = [](const SqlDeclaration& d) {
        if (!d.parameters.empty())
            throw Error(ErrorCode::type, "BLOB takes no parameters");
        return blobs::type();
    };
    a.install = blobs::install;
    a.can_convert = [](const Type& source, const Type& target) {
        return (source == text() && target == blobs::type()) || (source == blobs::type() && target == text());
    };
    a.convert = [](const Value& value, const Type& target, bool explicit_cast) -> Value {
        if (!explicit_cast)
            throw Error(ErrorCode::type, "TEXT/BLOB conversion requires explicit CAST");
        if (target == text()) {
            const auto data = blobs::bytes(value);
            if (data.empty())
                return std::string{};
            return std::string(reinterpret_cast<const char*>(data.data()), data.size());
        }
        const auto& text = std::get<std::string>(value);
        return blobs::value(std::as_bytes(std::span(text.data(), text.size())));
    };
    a.typed_literal = blobs::from_hex;
    a.operation = [](std::string_view op, std::span<const Type> types) -> std::optional<SqlOperation> {
        if (types.size() == 1 && types[0] == blobs::type()) {
            if (op == "length")
                return SqlOperation{"blob.length", {}, blobs::type(), true};
            if (op == "hex")
                return SqlOperation{"blob.hex", {}, blobs::type(), true};
        }
        return {};
    };
    a.repeatable = true;
    a.reorder_comparisons = true;
    return a;
}
} // namespace coresql::sql
