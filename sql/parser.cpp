#include "ast.hpp"
#include <charconv>
#include <cmath>

namespace coresql::sql::detail {
namespace {
struct Token {
    enum Kind { word, identifier, string, number, symbol, end } kind;
    std::string text;
    std::size_t at;
};
[[noreturn]] void error(std::size_t at, const std::string& message) {
    throw Error(ErrorCode::unsupported, "SQL byte " + std::to_string(at) + ": " + message);
}
bool alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool digit(char c) {
    return c >= '0' && c <= '9';
}
std::vector<Token> lex(std::string_view s) {
    if (s.size() > 1024 * 1024)
        error(0, "statement exceeds 1 MiB");
    std::vector<Token> out;
    for (std::size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            ++i;
            continue;
        }
        if (s.substr(i, 2) == "--") {
            while (i < s.size() && s[i] != '\n')
                ++i;
            continue;
        }
        if (s.substr(i, 2) == "/*") {
            auto end = s.find("*/", i + 2);
            if (end == s.npos)
                error(i, "unterminated comment");
            i = end + 2;
            continue;
        }
        auto start = i;
        if (c == '\'' || c == '"' || c == '`' || c == '[') {
            char close = c == '[' ? ']' : c;
            std::string text;
            ++i;
            bool done = false;
            while (i < s.size()) {
                char v = s[i++];
                if (v == close) {
                    if (i < s.size() && s[i] == close) {
                        ++i;
                        text += v;
                    } else {
                        done = true;
                        break;
                    }
                } else
                    text += v;
            }
            if (!done)
                error(start, "unterminated quoted token");
            // Identifiers are case insensitive, including quoted identifiers.
            if (c != '\'')
                for (char& v : text)
                    if (v >= 'A' && v <= 'Z')
                        v = char(v + ('a' - 'A'));
            out.push_back({c == '\'' ? Token::string : Token::identifier, std::move(text), start});
        } else if (alpha(c)) {
            while (i < s.size() && (alpha(s[i]) || digit(s[i])))
                ++i;
            std::string text(s.substr(start, i - start));
            for (char& v : text)
                if (v >= 'A' && v <= 'Z')
                    v = char(v + ('a' - 'A'));
            out.push_back({Token::word, std::move(text), start});
        } else if (digit(c) || (c == '.' && i + 1 < s.size() && digit(s[i + 1]))) {
            while (i < s.size() && digit(s[i]))
                ++i;
            if (i < s.size() && s[i] == '.') {
                ++i;
                while (i < s.size() && digit(s[i]))
                    ++i;
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                    ++i;
                while (i < s.size() && digit(s[i]))
                    ++i;
            }
            out.push_back({Token::number, std::string(s.substr(start, i - start)), start});
        } else {
            ++i;
            auto pair = s.substr(start, 2);
            if (pair == "<=" || pair == ">=" || pair == "<>" || pair == "!=" || pair == "==" || pair == "||")
                ++i;
            out.push_back({Token::symbol, std::string(s.substr(start, i - start)), start});
        }
        if (out.size() > 16384)
            error(start, "too many tokens");
    }
    out.push_back({Token::end, "", s.size()});
    return out;
}
struct Parser {
    std::vector<Token> tokens;
    const TypeAdapters& types;
    std::size_t cursor = 0, count = 0, depth = 0, query_depth = 0;
    const Token& peek() const { return tokens[cursor]; }
    bool is(std::string_view s) const {
        return (peek().kind == Token::word || peek().kind == Token::symbol) && peek().text == s;
    }
    bool take(std::string_view s) {
        if (is(s)) {
            ++cursor;
            return true;
        }
        return false;
    }
    void need(std::string_view s) {
        if (!take(s))
            error(peek().at, "expected " + std::string(s));
    }
    std::string name() {
        if (peek().kind != Token::word && peek().kind != Token::identifier)
            error(peek().at, "expected identifier");
        return tokens[cursor++].text;
    }
    Node node(Node::Kind kind, std::string op = {}, std::vector<Node> args = {}) {
        Node n;
        n.kind = kind;
        n.name = std::move(op);
        for (auto& a : args) {
            if (kind == Node::binary && (n.name == "and" || n.name == "or") && a.kind == kind &&
                a.name == n.name) {
                for (auto& child : a.args)
                    n.args.push_back(std::move(child));
            } else
                n.args.push_back(std::move(a));
        }
        for (const auto& a : n.args)
            n.depth = std::max(n.depth, a.depth + 1);
        if (n.depth > 64)
            error(peek().at, "expression tree exceeds 64 levels");
        return n;
    }
    static int precedence(std::string_view op) {
        if (op == "or")
            return 1;
        if (op == "and")
            return 2;
        if (op == "=" || op == "==" || op == "!=" || op == "<>" || op == "<" || op == ">" || op == "<=" ||
            op == ">=" || op == "like" || op == "between" || op == "in" || op == "is")
            return 3;
        if (op == "&")
            return 4;
        if (op == "+" || op == "-")
            return 5;
        if (op == "*" || op == "/" || op == "%")
            return 6;
        if (op == "||")
            return 7;
        return 0;
    }
    Type extension_type(const SqlTypeAdapter& adapter, std::string_view name, bool cast = false) {
        std::vector<std::uint64_t> arguments;
        if (take("(")) {
            do {
                auto token = peek();
                std::uint64_t value = 0;
                auto [end, ec] =
                    std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
                if (token.kind != Token::number || ec != std::errc{} ||
                    end != token.text.data() + token.text.size())
                    error(token.at, "Expected unsigned SQL type parameter");
                ++cursor;
                arguments.push_back(value);
            } while (take(","));
            need(")");
        }
        auto target = adapter.declaration({name, arguments, cast});
        if (target.id != adapter.type_id || target.version != adapter.version)
            throw Error(ErrorCode::type, "SQL adapter returned wrong declaration type");
        return target;
    }
    Node expr(int minimum = 1) {
        if (++depth > 64)
            error(peek().at, "expression nesting exceeds 64");
        Node n;
        auto token = peek();
        bool existence = take("exists");
        if (existence)
            need("(");
        if (existence || take("(")) {
            if (is("select") || is("with")) {
                n = node(existence ? Node::exists : Node::subquery);
                n.query = std::make_shared<Select>(select());
                for (const auto& a : n.query->projection)
                    n.depth = std::max(n.depth, a.depth + 1);
                for (const auto* a : {&n.query->where, &n.query->limit, &n.query->offset})
                    if (*a)
                        n.depth = std::max(n.depth, (**a).depth + 1);
                for (const auto& [key, descending] : n.query->order) {
                    (void)descending;
                    n.depth = std::max(n.depth, key.depth + 1);
                }
                if (n.depth > 64)
                    error(peek().at, "subquery tree exceeds 64 levels");
            } else {
                if (existence)
                    error(peek().at, "EXISTS requires SELECT");
                n = expr();
            }
            need(")");
        } else if (take("extract")) {
            need("(");
            auto part = name();
            need("from");
            auto value = expr();
            need(")");
            n = node(Node::function, "sql.extract." + part, {std::move(value)});
        } else if (take("substring")) {
            need("(");
            std::vector<Node> args{expr()};
            if (take("from")) {
                args.push_back(expr());
                if (take("for"))
                    args.push_back(expr());
            } else {
                need(",");
                args.push_back(expr());
                if (take(","))
                    args.push_back(expr());
            }
            need(")");
            n = node(Node::function, "text.substring", std::move(args));
        } else if (take("cast")) {
            need("(");
            auto value = expr();
            need("as");
            auto type = name();
            if (auto adapter = types.named(type)) {
                auto target = extension_type(*adapter, type, true);
                need(")");
                n = node(Node::function, "sql.cast_type", {std::move(value)});
                n.value = Null(target);
                if (adapter->fold_text_cast && n.args[0].kind == Node::literal)
                    if (auto text = std::get_if<std::string>(&n.args[0].value)) {
                        try {
                            auto converted = types.convert(*text, target, true);
                            auto tree_depth = n.depth;
                            n = Node{};
                            n.depth = tree_depth;
                            n.value = std::move(converted);
                        } catch (const Error&) {
                        } // Invalid casts remain lazy.
                    }
            } else {
                if (type != "numeric")
                    error(peek().at, "Unsupported CAST type");
                need(")");
                n = node(Node::function, "sql.cast_" + type, {std::move(value)});
            }
        } else if (peek().kind == Token::word && tokens[cursor + 1].kind == Token::string &&
                   types.named(peek().text)) {
            const auto& adapter = *types.named(tokens[cursor++].text);
            if (!adapter.typed_literal)
                error(peek().at, "SQL type has no string literal syntax");
            n.value = adapter.typed_literal(tokens[cursor++].text);
            if (type_of(n.value).id != adapter.type_id || type_of(n.value).version != adapter.version)
                throw Error(ErrorCode::type, "SQL adapter returned wrong literal type");
        } else if (take("case")) {
            bool searched = is("when");
            std::vector<Node> args;
            if (!searched)
                args.push_back(expr());
            need("when");
            do {
                args.push_back(expr());
                need("then");
                args.push_back(expr());
            } while (take("when"));
            if (take("else"))
                args.push_back(expr());
            need("end");
            n = node(searched ? Node::case_when : Node::case_match, {}, std::move(args));
        } else if (take("not"))
            n = node(Node::unary, "not", {expr(3)});
        else if (take("-")) {
            if (peek().kind == Token::number && peek().text == "9223372036854775808") {
                ++cursor;
                n.value = std::int64_t{INT64_MIN};
            } else
                n = node(Node::unary, "-", {expr(8)});
        } else if (take("+"))
            n = expr(8);
        else if (take("*"))
            n = node(Node::star);
        else if (take("?")) {
            std::size_t index = count + 1;
            if (peek().kind == Token::number && peek().at == token.at + 1) {
                auto t = tokens[cursor++];
                auto [p, ec] = std::from_chars(t.text.data(), t.text.data() + t.text.size(), index);
                if (ec != std::errc{} || p != t.text.data() + t.text.size())
                    error(t.at, "invalid parameter index");
            }
            if (!index || index > 4096)
                error(token.at, "parameter index must be 1..4096");
            count = std::max(count, index);
            n = node(Node::parameter);
            n.position = index - 1;
        } else if (token.kind == Token::number) {
            ++cursor;
            n.numeric_spelling = token.text;
            if (token.text.find_first_of(".eE") == std::string::npos) {
                std::int64_t v;
                auto [p, ec] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), v);
                if (ec != std::errc{} || p != token.text.data() + token.text.size())
                    error(token.at, "integer literal out of range");
                n.value = v;
            } else {
                double v;
                auto [p, ec] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), v);
                if (ec != std::errc{} || p != token.text.data() + token.text.size() || !std::isfinite(v))
                    error(token.at, "invalid real literal");
                n.value = v;
            }
        } else if (token.kind == Token::string) {
            ++cursor;
            n.value = token.text;
        } else if (take("null")) {
            n.value = Null(integer());
        } else {
            auto word = name();
            if (take("(")) {
                bool distinct = take("distinct");
                std::vector<Node> args;
                if (!take(")")) {
                    do {
                        args.push_back(expr());
                    } while (take(","));
                    need(")");
                }
                n = node(Node::function, word, std::move(args));
                n.distinct = distinct;
            } else {
                n = node(Node::column, word);
                if (take(".")) {
                    n.qualifier = word;
                    n.name = name();
                }
            }
        }
        while (true) {
            bool negated = is("not") && cursor + 1 < tokens.size() &&
                           tokens[cursor + 1].kind == Token::word &&
                           (tokens[cursor + 1].text == "between" || tokens[cursor + 1].text == "in" ||
                            tokens[cursor + 1].text == "like");
            int p = negated                                                        ? 3
                    : (peek().kind == Token::word || peek().kind == Token::symbol) ? precedence(peek().text)
                                                                                   : 0;
            if (!p || p < minimum)
                break;
            if (negated)
                ++cursor;
            auto op = tokens[cursor++].text;
            if (op == "is") {
                bool negation = take("not");
                need("null");
                n = node(Node::unary, "is_null", {std::move(n)});
                if (negation)
                    n = node(Node::unary, "not", {std::move(n)});
            } else if (op == "in") {
                need("(");
                auto value = std::move(n);
                n = node(Node::membership, {}, {std::move(value)});
                if (is("select") || is("with"))
                    n.query = std::make_shared<Select>(select());
                else if (!is(")")) {
                    do {
                        n.args.push_back(expr());
                    } while (take(","));
                }
                need(")");
            } else if (op == "between") {
                auto lo = expr(p + 1);
                need("and");
                auto hi = expr(p + 1);
                n = node(Node::binary, op, {std::move(n), std::move(lo), std::move(hi)});
            } else {
                n = node(Node::binary, op, {std::move(n), expr(p + 1)});
            }
            if (negated)
                n = node(Node::unary, "not", {std::move(n)});
        }
        --depth;
        return n;
    }
    Select select_core() {
        need("select");
        Select q;
        q.distinct = take("distinct");
        do {
            q.projection.push_back(expr());
            q.aliases.push_back(take("as") ? name() : std::string{});
        } while (take(","));
        if (take("from")) {
            JoinKind kind = JoinKind::inner;
            bool explicit_join = false;
            do {
                Source source;
                source.kind = kind;
                source.explicit_join = explicit_join;
                if (take("(")) {
                    source.query = std::make_shared<Select>(select());
                    need(")");
                } else
                    source.table = name();
                source.alias = source.table;
                if (take("as"))
                    source.alias = name();
                else if (peek().kind == Token::word && !is("where") && !is("order") && !is("limit") &&
                         !is("group") && !is("having") && !is("join") && !is("inner") && !is("union") &&
                         !is("intersect") && !is("except") && !is("cross") && !is("on") && !is("left") &&
                         !is("right") && !is("full") && !is("outer") && !is("natural") && !is("using") &&
                         !is("returning"))
                    source.alias = name();
                if (source.alias.empty())
                    error(peek().at, "Derived relation requires an alias");
                if (take("(")) {
                    do {
                        source.columns.push_back(name());
                    } while (take(","));
                    need(")");
                }
                if (take("on")) {
                    if (!explicit_join)
                        error(peek().at, "ON requires JOIN");
                    source.on = expr();
                }
                q.sources.push_back(std::move(source));
                kind = JoinKind::inner;
                explicit_join = false;
                if (take(","))
                    continue;
                explicit_join = true;
                if (take("left"))
                    kind = JoinKind::left;
                else if (take("right"))
                    kind = JoinKind::right;
                else if (take("full"))
                    kind = JoinKind::full;
                else if (take("inner") || take("cross")) {
                    need("join");
                    continue;
                } else if (take("join"))
                    continue;
                else
                    break;
                take("outer");
                need("join");
                continue;
                break;
            } while (true);
        }
        if (take("where")) {
            auto condition = expr();
            q.where = q.where ? node(Node::binary, "and", {std::move(*q.where), std::move(condition)})
                              : std::move(condition);
        }
        if (take("group")) {
            need("by");
            do {
                q.group.push_back(expr());
            } while (take(","));
        }
        if (take("having"))
            q.having = expr();
        return q;
    }
    Select select() {
        if (++query_depth > 64)
            error(peek().at, "Query nesting exceeds 64");
        std::vector<CommonTable> ctes;
        if (take("with")) {
            if (take("recursive"))
                error(peek().at, "Recursive CTEs are unsupported");
            do {
                CommonTable cte;
                cte.name = name();
                if (take("(")) {
                    do {
                        cte.columns.push_back(name());
                    } while (take(","));
                    need(")");
                }
                need("as");
                need("(");
                cte.query = std::make_shared<Select>(select());
                need(")");
                ctes.push_back(std::move(cte));
            } while (take(","));
        }
        auto q = select_core();
        q.ctes = std::move(ctes);
        while (is("union") || is("intersect") || is("except")) {
            auto op = tokens[cursor++].text;
            auto kind = op == "union" ? (take("all") ? SetOperation::union_all : SetOperation::union_distinct)
                        : op == "intersect" ? SetOperation::intersect
                                            : SetOperation::except;
            q.compounds.emplace_back(kind, std::make_shared<Select>(select_core()));
        }
        if (take("order")) {
            need("by");
            do {
                auto key = expr();
                bool descending = take("desc");
                if (!descending)
                    take("asc");
                q.order.emplace_back(std::move(key), descending);
            } while (take(","));
        }
        if (take("limit")) {
            q.limit = expr();
            if (take("offset"))
                q.offset = expr();
        }
        --query_depth;
        return q;
    }
    Field field() {
        Field f;
        f.name = name();
        f.type = any_type();
        if (const auto* adapter = types.named(peek().text); adapter && peek().kind == Token::word) {
            auto type_name = tokens[cursor++].text;
            f.type = extension_type(*adapter, type_name);
        }
        while (true) {
            if (take("primary")) {
                need("key");
                f.unique = true;
                f.primary = true;
            } else if (take("unique"))
                f.unique = true;
            else if (take("not")) {
                need("null");
                f.nullable = false;
            } else if (take("default"))
                f.value = expr();
            else
                break;
        }
        return f;
    }
    Statement statement() {
        Statement s;
        if (is("select") || is("with")) {
            s.kind = Statement::select;
            s.query = std::make_shared<Select>(select());
        } else if (take("begin")) {
            s.kind = Statement::begin;
            take("transaction");
        } else if (take("commit") || take("end")) {
            s.kind = Statement::commit;
            take("transaction");
        } else if (take("rollback")) {
            s.kind = Statement::rollback;
            take("transaction");
            if (take("to")) {
                take("savepoint");
                s.kind = Statement::rollback_to;
                s.table = name();
            }
        } else if (take("savepoint")) {
            s.kind = Statement::savepoint;
            s.table = name();
        } else if (take("release")) {
            take("savepoint");
            s.kind = Statement::release;
            s.table = name();
        } else if (take("drop")) {
            if (take("table"))
                s.kind = Statement::drop_table;
            else {
                need("index");
                s.kind = Statement::drop_index;
            }
            if (take("if")) {
                need("exists");
                s.if_exists = true;
            }
            s.table = name();
        } else if (take("vacuum"))
            s.kind = Statement::vacuum;
        else if (take("analyze"))
            s.kind = Statement::analyze;
        else if (take("pragma")) {
            need("integrity_check");
            s.kind = Statement::integrity;
        } else if (take("create")) {
            bool unique = take("unique");
            if (take("table")) {
                if (unique)
                    error(peek().at, "UNIQUE TABLE unsupported");
                s.kind = Statement::create_table;
                if (take("if")) {
                    need("not");
                    need("exists");
                    s.if_not_exists = true;
                }
                s.table = name();
                need("(");
                do {
                    s.fields.push_back(field());
                } while (take(","));
                need(")");
                if (take("without")) {
                    need("rowid");
                    if (std::count_if(s.fields.begin(), s.fields.end(),
                                      [](const Field& f) { return f.primary && f.type == text(); }) != 1)
                        error(peek().at, "WITHOUT ROWID currently requires a TEXT primary key");
                }
            } else {
                need("index");
                s.kind = Statement::create_index;
                if (take("if")) {
                    need("not");
                    need("exists");
                    s.if_not_exists = true;
                }
                s.index.name = name();
                s.index.unique = unique;
                need("on");
                s.table = name();
                need("(");
                do {
                    s.index.columns.push_back(name());
                    bool desc = take("desc");
                    if (!desc)
                        take("asc");
                    s.index.descending.push_back(desc);
                } while (take(","));
                need(")");
            }
        } else if (take("alter")) {
            need("table");
            s.table = name();
            if (take("rename")) {
                if (take("to")) {
                    s.kind = Statement::rename_table;
                    s.replacement = name();
                } else {
                    take("column");
                    s.kind = Statement::rename_column;
                    s.old_column = name();
                    need("to");
                    s.replacement = name();
                }
            } else {
                need("add");
                take("column");
                s.kind = Statement::add_column;
                s.fields.push_back(field());
            }
        } else if (is("insert") || is("replace")) {
            s.replace = take("replace");
            if (!s.replace) {
                need("insert");
                if (take("or")) {
                    need("replace");
                    s.replace = true;
                }
            }
            s.kind = Statement::insert;
            need("into");
            s.table = name();
            if (take("(")) {
                do {
                    s.columns.push_back(name());
                } while (take(","));
                need(")");
            }
            if (take("default")) {
                need("values");
                s.default_values = true;
            } else if (take("values")) {
                do {
                    need("(");
                    std::vector<Node> row;
                    do {
                        row.push_back(expr());
                    } while (take(","));
                    need(")");
                    s.rows.push_back(std::move(row));
                } while (take(","));
            } else
                s.query = std::make_shared<Select>(select());
        } else if (take("update")) {
            s.kind = Statement::update;
            s.table = name();
            need("set");
            do {
                auto c = name();
                need("=");
                s.assignments.emplace_back(c, expr());
            } while (take(","));
            if (take("where"))
                s.where = expr();
        } else if (take("delete")) {
            s.kind = Statement::erase;
            need("from");
            s.table = name();
            if (take("where"))
                s.where = expr();
        } else
            error(peek().at, "unsupported statement");
        if (s.kind == Statement::insert && take("returning")) {
            do {
                s.returning.push_back(expr());
                s.returning_aliases.push_back(take("as") ? name() : std::string{});
            } while (take(","));
        }
        take(";");
        if (peek().kind != Token::end)
            error(peek().at, "unsupported syntax or multiple statements: " + peek().text);
        s.parameters = count;
        return s;
    }
};
} // namespace
Statement parse(std::string_view text, const TypeAdapters& types) {
    return Parser{lex(text), types}.statement();
}
std::vector<coresql::sql::Statement> script(std::string_view text, const TypeAdapters& types) {
    std::vector<coresql::sql::Statement> result;
    std::size_t start = 0;
    bool content = false;
    for (const auto& token : lex(text)) {
        if (token.kind == Token::end || (token.kind == Token::symbol && token.text == ";")) {
            if (content)
                result.emplace_back(text.substr(start, token.at - start), types);
            start = token.at + 1;
            content = false;
        } else
            content = true;
    }
    return result;
}
} // namespace coresql::sql::detail
namespace coresql::sql {
Statement::Statement(std::string_view text, const TypeAdapters& types)
    : parsed_(std::make_shared<const detail::Statement>(detail::parse(text, types))), adapters_(types) {
}
std::size_t Statement::parameter_count() const {
    if (!parsed_)
        throw Error(ErrorCode::state, "SQL statement was moved from");
    return parsed_->parameters;
}
std::vector<Statement> prepare_script(std::string_view text, const TypeAdapters& types) {
    return detail::script(text, types);
}
} // namespace coresql::sql
