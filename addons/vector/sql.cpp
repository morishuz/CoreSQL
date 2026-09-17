#include "coresql/sql_types.hpp"
#include "coresql/vector.hpp"
#include <charconv>
#include <cmath>

namespace coresql::sql {
namespace {
bool vector_type(const Type& t) {
    return t.id == vectors::type().id && t.version == 1;
}
Value parse(std::string_view s, const Type& target) {
    auto space = [&] {
        while (!s.empty() &&
               (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
            s.remove_prefix(1);
    };
    auto fail = [] { throw Error(ErrorCode::type, "Expected bracketed finite float32 vector"); };
    space();
    if (s.empty() || s.front() != '[')
        fail();
    s.remove_prefix(1);
    space();
    std::vector<float> values;
    if (!s.empty() && s.front() != ']') {
        while (true) {
            float value = 0;
            auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
            if (ec != std::errc{} || end == s.data() || !std::isfinite(value))
                fail();
            values.push_back(value);
            s.remove_prefix(static_cast<std::size_t>(end - s.data()));
            space();
            if (s.empty() || s.front() != ',')
                break;
            s.remove_prefix(1);
            space();
        }
    }
    if (s.empty() || s.front() != ']')
        fail();
    s.remove_prefix(1);
    space();
    if (!s.empty())
        fail();
    return vectors::value(values, vectors::dimensions(target));
}
std::string format(const Value& v) {
    std::string out = "[";
    for (float f : vectors::elements(v)) {
        if (out.size() > 1)
            out += ',';
        char buffer[64];
        auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), f);
        if (ec != std::errc{})
            throw Error(ErrorCode::type, "Cannot format vector element");
        out.append(buffer, end);
    }
    return out + ']';
}
} // namespace
SqlTypeAdapter vector_adapter() {
    SqlTypeAdapter a;
    a.type_id = vectors::type().id;
    a.names = {"vector"};
    a.declaration = [](const SqlDeclaration& declaration) {
        auto p = declaration.parameters;
        if (p.size() > 1)
            throw Error(ErrorCode::type, "VECTOR accepts at most one dimension");
        return p.empty() ? vectors::type() : vectors::type(p[0]);
    };
    a.install = [](Registry& r) {
        vectors::install(r);
        r.add(Function{
            "sql.vector",
            [](std::span<const Type> t) {
                if (t.size() != 1 || t[0] != text())
                    throw Error(ErrorCode::type, "VECTOR expects text");
                return vectors::type();
            },
            [](std::span<const Value> v) { return parse(std::get<std::string>(v[0]), vectors::type()); }});
    };
    a.can_convert = [](const Type& s, const Type& t) {
        return (vector_type(t) && (s == text() || s.id == "sql.value" || vector_type(s))) ||
               (vector_type(s) && t == text());
    };
    a.convert = [](const Value& v, const Type& t, bool) -> Value {
        if (t == text())
            return format(v);
        if (auto s = std::get_if<std::string>(&v))
            return parse(*s, t);
        return vectors::value(vectors::elements(v), vectors::dimensions(t));
    };
    a.typed_literal = [](std::string_view s) { return parse(s, vectors::type()); };
    a.operation = [](std::string_view op, std::span<const Type>) -> std::optional<SqlOperation> {
        if (op == "vector")
            return SqlOperation{"sql.vector", {}, text(), true};
        if (op == "vector_squared_l2")
            return SqlOperation{"vector.squared_l2", {}, vectors::type(), true};
        return {};
    };
    a.common_type = [](std::span<const Type> types) -> std::optional<Type> {
        if (types.empty() || !std::all_of(types.begin(), types.end(), vector_type))
            return {};
        return std::all_of(types.begin(), types.end(), [&](const Type& t) { return t == types[0]; })
                   ? types[0]
                   : vectors::type();
    };
    a.fold_text_cast = true;
    a.repeatable = true;
    // Equality is total, but vectors deliberately have no ordering callback.
    return a;
}
} // namespace coresql::sql
