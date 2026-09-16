#include "coresql/json.hpp"
#include <charconv>
#include <cmath>
#include <set>

namespace coresql::json {
namespace {
[[noreturn]] void invalid() { throw Error(ErrorCode::type, "Invalid JSON (depth limit 64; duplicate object keys rejected)"); }
std::string_view bytes(ByteView v) { return {reinterpret_cast<const char*>(v.data()), v.size()}; }
void utf8(std::string& out, unsigned c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
    else {
        if (c >= 0x10000) { out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 63)); }
        else out += static_cast<char>(0xe0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63));
    }
}
// Validate raw UTF-8, excluding overlong sequences, surrogates and > U+10FFFF.
std::size_t character(std::string_view s, std::size_t i) {
    auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return i + 1;
    unsigned n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!n || s.size() - i < n) invalid();
    unsigned code = c & (0x7fU >> n);
    for (unsigned j = 1; j < n; ++j) {
        auto next = static_cast<unsigned char>(s[i + j]);
        if ((next & 0xc0) != 0x80) invalid();
        code = (code << 6) | (next & 63);
    }
    if ((n == 3 && code < 0x800) || (n == 4 && code < 0x10000) ||
        (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) invalid();
    return i + n;
}
std::vector<std::string> pointer_parts(std::string_view pointer) {
    for (std::size_t i = 0; i < pointer.size();) i = character(pointer, i);
    std::vector<std::string> parts;
    if (pointer.empty()) return parts;
    if (pointer.front() != '/') throw Error(ErrorCode::type, "JSON pointer must be empty or begin with /");
    parts.emplace_back();
    for (std::size_t i = 1; i < pointer.size(); ++i) {
        char c = pointer[i];
        if (c == '/') parts.emplace_back();
        else if (c == '~') {
            if (++i == pointer.size() || (pointer[i] != '0' && pointer[i] != '1'))
                throw Error(ErrorCode::type, "Invalid JSON pointer escape");
            parts.back() += pointer[i] == '0' ? '~' : '/';
        } else parts.back() += c;
    }
    return parts;
}
class Parser {
    std::string_view input;
    std::span<const std::string> path;
    std::size_t position = 0;
    Type key_type;
    std::vector<Value> result;
    char peek() const { return position < input.size() ? input[position] : '\0'; }
    void whitespace() { while (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r') ++position; }
    bool take(char c) { if (peek() != c || position == input.size()) return false; ++position; return true; }
    void require(char c) { if (!take(c)) invalid(); }
    void word(std::string_view w) {
        if (input.substr(position, w.size()) != w) invalid();
        position += w.size();
    }
    unsigned hex4() {
        unsigned code = 0;
        for (unsigned i = 0; i < 4; ++i) {
            char c = peek(); unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else invalid();
            ++position; code = code * 16 + digit;
        }
        return code;
    }
    std::string string() {
        require('"'); std::string out;
        while (!take('"')) {
            if (static_cast<unsigned char>(peek()) < 0x20) invalid();
            if (take('\\')) {
                char c = peek(); if (position == input.size()) invalid(); ++position;
                switch (c) {
                case '"': case '\\': case '/': out += c; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    auto code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        require('\\'); require('u'); auto low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) invalid();
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    } else if (code >= 0xdc00 && code <= 0xdfff) invalid();
                    utf8(out, code); break;
                }
                default: invalid();
                }
            } else {
                auto end = character(input, position);
                out.append(input.substr(position, end - position)); position = end;
            }
        }
        return out;
    }
    void number(bool selected) {
        auto start = position;
        take('-');
        auto digit = [&] { return peek() >= '0' && peek() <= '9'; };
        if (!take('0')) { if (peek() < '1' || peek() > '9') invalid(); while (digit()) ++position; }
        bool integral = true;
        if (take('.')) { integral = false; if (!digit()) invalid(); while (digit()) ++position; }
        if (take('e') || take('E')) {
            integral = false; if (!take('+')) take('-');
            if (!digit()) invalid(); while (digit()) ++position;
        }
        if (!selected) return;
        const char* begin = input.data() + start; const char* end = input.data() + position;
        if (key_type == integer() && integral) {
            std::int64_t n; auto parsed = std::from_chars(begin, end, n);
            if (parsed.ec == std::errc{} && parsed.ptr == end) result.emplace_back(n);
        } else if (key_type == real()) {
            double n; auto parsed = std::from_chars(begin, end, n);
            if (parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(n)) result.emplace_back(n);
        }
    }
    void parse(unsigned depth, std::optional<std::size_t> part) {
        if (depth > 64) invalid();
        whitespace(); bool selected = part && *part == path.size();
        if (peek() == '"') {
            auto s = string(); if (selected && key_type == text()) result.emplace_back(std::move(s));
        } else if (take('{')) {
            std::set<std::string> seen;
            whitespace(); if (take('}')) return;
            do {
                whitespace(); auto key = string();
                if (!seen.insert(key).second) invalid();
                whitespace(); require(':');
                parse(depth + 1, part && !selected && path[*part] == key ? std::optional(*part + 1) : std::nullopt);
                whitespace(); if (take('}')) return;
                require(',');
            } while (true);
        } else if (take('[')) {
            whitespace(); if (take(']')) return;
            std::size_t index = 0;
            do {
                parse(depth + 1, part && !selected && path[*part] == std::to_string(index) ? std::optional(*part + 1) : std::nullopt);
                ++index; whitespace(); if (take(']')) return;
                require(',');
            } while (true);
        } else if (peek() == 't') word("true");
        else if (peek() == 'f') word("false");
        else if (peek() == 'n') word("null");
        else number(selected);
    }
public:
    Parser(std::string_view input, std::span<const std::string> path = {}, Type key_type = text())
        : input(input), path(path), key_type(std::move(key_type)) {}
    std::vector<Value> run(bool extract = false) {
        parse(0, extract ? std::optional<std::size_t>(0) : std::nullopt);
        whitespace(); if (position != input.size()) invalid();
        return std::move(result);
    }
};
}
Type type() { return {"json.document", 1, {}}; }
Value value(std::string_view document) {
    Parser(document).run();
    auto data = std::as_bytes(std::span(document.data(), document.size()));
    return Opaque(type(), Bytes(data.begin(), data.end()));
}
void install(Registry& registry) {
    EncodedTypeAddon addon;
    addon.id = type().id;
    addon.validate_type = [](ByteView p) { if (!p.empty()) throw Error(ErrorCode::type, "JSON type takes no parameters"); };
    addon.validate_value = [](ByteView, ByteView v) { Parser(bytes(v)).run(); };
    // Document equality/ordering is intentionally unspecified; query extracted keys.
    registry.add(std::move(addon));
}
KeyExtractor property(std::string name, std::string_view pointer, Type key_type) {
    if (key_type != text() && key_type != integer() && key_type != real())
        throw Error(ErrorCode::unsupported, "JSON property requires a native scalar key type");
    auto path = pointer_parts(pointer);
    return {std::move(name), type(), key_type, [path = std::move(path), key_type](const Value& v) {
        return Parser(bytes(std::get<Opaque>(v).bytes()), path, key_type).run(true);
    }};
}
}
