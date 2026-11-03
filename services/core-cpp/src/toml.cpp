// toml.cpp — strict in-tree TOML parser. See toml.hpp for the contract.
#include "toml.hpp"

#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace parlay::toml {

ParseError::ParseError(const std::string& source, int line,
                       const std::string& detail)
    : std::runtime_error("toml: " + source + ":" + std::to_string(line) +
                         ": " + detail),
      source_(source),
      line_(line) {}

Value Value::makeString(std::string v) {
    Value out;
    out.type = Type::String;
    out.str = std::move(v);
    return out;
}
Value Value::makeInteger(int64_t v) {
    Value out;
    out.type = Type::Integer;
    out.integer = v;
    return out;
}
Value Value::makeFloat(double v) {
    Value out;
    out.type = Type::Float;
    out.floating = v;
    return out;
}
Value Value::makeBoolean(bool v) {
    Value out;
    out.type = Type::Boolean;
    out.boolean = v;
    return out;
}
Value Value::makeArray(std::vector<Value> v) {
    Value out;
    out.type = Type::Array;
    out.array = std::move(v);
    return out;
}
Value Value::makeTable(std::map<std::string, Value> v) {
    Value out;
    out.type = Type::Table;
    out.table = std::move(v);
    return out;
}

std::string Value::typeName() const {
    switch (type) {
        case Type::String: return "string";
        case Type::Integer: return "integer";
        case Type::Float: return "float";
        case Type::Boolean: return "boolean";
        case Type::Array: return "array";
        case Type::Table: return "table";
    }
    return "unknown";
}

namespace {

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

}  // namespace

const Value* Document::find(const std::string& dotted_path) const {
    const Value* cur = &root_;
    for (const std::string& part : splitPath(dotted_path)) {
        if (!cur->isTable()) return nullptr;
        auto it = cur->table.find(part);
        if (it == cur->table.end()) return nullptr;
        cur = &it->second;
    }
    return cur;
}

namespace {
// Type errors from the accessors carry no file line (the document was
// already parsed); line 0 marks "no source position".
[[noreturn]] void wrongType(const std::string& path, const std::string& want,
                            const Value& got) {
    throw ParseError("<document>", 0,
                     "key '" + path + "' must be " + want + ", got " +
                         got.typeName());
}
}  // namespace

std::string Document::getString(const std::string& path) const {
    const Value* v = find(path);
    if (!v) throw ParseError("<document>", 0, "missing key '" + path + "'");
    if (!v->isString()) wrongType(path, "a string", *v);
    return v->str;
}

int64_t Document::getInteger(const std::string& path) const {
    const Value* v = find(path);
    if (!v) throw ParseError("<document>", 0, "missing key '" + path + "'");
    if (!v->isInteger()) wrongType(path, "an integer", *v);
    return v->integer;
}

double Document::getNumber(const std::string& path) const {
    const Value* v = find(path);
    if (!v) throw ParseError("<document>", 0, "missing key '" + path + "'");
    if (v->isInteger()) return static_cast<double>(v->integer);
    if (!v->isFloat()) wrongType(path, "a number", *v);
    return v->floating;
}

bool Document::getBoolean(const std::string& path) const {
    const Value* v = find(path);
    if (!v) throw ParseError("<document>", 0, "missing key '" + path + "'");
    if (!v->isBoolean()) wrongType(path, "a boolean", *v);
    return v->boolean;
}

std::vector<std::string> Document::getStringArray(
    const std::string& path) const {
    const Value* v = find(path);
    if (!v) throw ParseError("<document>", 0, "missing key '" + path + "'");
    if (!v->isArray()) wrongType(path, "an array of strings", *v);
    std::vector<std::string> out;
    for (size_t i = 0; i < v->array.size(); ++i) {
        if (!v->array[i].isString())
            throw ParseError("<document>", 0,
                             "key '" + path + "' element " +
                                 std::to_string(i) +
                                 " must be a string, got " +
                                 v->array[i].typeName());
        out.push_back(v->array[i].str);
    }
    return out;
}

const Value* Document::getTable(const std::string& path) const {
    const Value* v = find(path);
    if (!v) return nullptr;
    if (!v->isTable()) wrongType(path, "a table", *v);
    return v;
}

// ---------------------------------------------------------------- parser

namespace {

class Parser {
public:
    Parser(const std::string& text, const std::string& source)
        : text_(text), source_(source) {}

    Document run() {
        Value root = Value::makeTable({});
        // The table currently receiving key/value pairs. The root table
        // itself is never "defined" by a header, so it can always
        // receive keys; a header-defined table may receive keys only
        // once (redefining it is an error).
        Value* current = &root;
        section_path_.clear();
        skipSpaceAndComments(/*newlines_ok=*/true);
        while (pos_ < text_.size()) {
            if (peek() == '[') {
                current = &parseTableHeader(root);
            } else {
                parseKeyValue(*current);
            }
            skipSpaceAndComments(/*newlines_ok=*/true);
        }
        return Document(std::move(root));
    }

private:
    const std::string& text_;
    const std::string& source_;
    size_t pos_ = 0;
    int line_ = 1;

    char peek(size_t off = 0) const {
        return pos_ + off < text_.size() ? text_[pos_ + off] : '\0';
    }
    char take() {
        char c = peek();
        if (c) {
            ++pos_;
            if (c == '\n') ++line_;
        }
        return c;
    }
    bool eof() const { return pos_ >= text_.size(); }

    [[noreturn]] void fail(const std::string& detail) const {
        throw ParseError(source_, line_, detail);
    }
    [[noreturn]] void failAt(int line, const std::string& detail) const {
        throw ParseError(source_, line, detail);
    }

    // Whitespace/comments. When newlines_ok is false (inside a table
    // header / key-value line) a newline ends the construct.
    void skipInlineSpace() {
        while (peek() == ' ' || peek() == '\t') take();
    }
    void skipSpaceAndComments(bool newlines_ok) {
        for (;;) {
            char c = peek();
            if (c == ' ' || c == '\t' || (newlines_ok && (c == '\n' || c == '\r'))) {
                take();
            } else if (c == '#') {
                while (!eof() && peek() != '\n') take();
            } else {
                return;
            }
        }
    }

    // Expect end of line: optional spaces, optional comment, then a
    // newline or EOF. Anything else is trailing garbage.
    void expectEol(const std::string& what) {
        skipInlineSpace();
        if (peek() == '#') {
            while (!eof() && peek() != '\n') take();
        }
        if (eof()) return;
        if (peek() == '\r' && peek(1) == '\n') {
            take();
            take();
            return;
        }
        if (peek() == '\n') {
            take();
            return;
        }
        fail("expected end of line after " + what + ", found '" +
             std::string(1, peek()) + "'");
    }

    std::string parseBareKey() {
        std::string key;
        while (true) {
            char c = peek();
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-') {
                key += take();
            } else {
                break;
            }
        }
        if (key.empty())
            fail("expected a key, found '" + std::string(1, peek()) + "'");
        return key;
    }

    // One key segment: bare, "basic", or 'literal'.
    std::string parseKeySegment() {
        skipInlineSpace();
        char c = peek();
        if (c == '"') return parseBasicStringFull();
        if (c == '\'') return parseLiteralString();
        return parseBareKey();
    }

    std::vector<std::string> parseKey() {
        std::vector<std::string> parts;
        parts.push_back(parseKeySegment());
        for (;;) {
            skipInlineSpace();
            if (peek() != '.') break;
            take();
            parts.push_back(parseKeySegment());
        }
        return parts;
    }

    // Navigate/create nested tables for a dotted key, creating
    // implicit intermediate tables. `prefix` is the enclosing
    // section's root-relative path ("" at the root); `defined` records
    // the full path of every intermediate table created by a dotted
    // key, because TOML forbids defining such a table again with a
    // [header]. Returns the table that receives the final key segment.
    Value* navigate(Value& root, const std::vector<std::string>& parts,
                    const std::string& prefix,
                    std::map<std::string, bool>& defined) {
        Value* cur = &root;
        std::string path = prefix;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            if (!path.empty()) path += ".";
            path += parts[i];
            auto it = cur->table.find(parts[i]);
            if (it == cur->table.end()) {
                auto inserted = cur->table.emplace(
                    parts[i], Value::makeTable({}));
                cur = &inserted.first->second;
                defined[path] = true;  // dotted-key table: no header later
            } else {
                if (!it->second.isTable())
                    fail("key '" + path +
                         "' is already defined as a value, cannot use "
                         "it as a table");
                cur = &it->second;
            }
        }
        return cur;
    }

    // Insert a finished value. Duplicate definition is an error.
    void insertValue(Value& table, const std::string& key, Value value,
                     const std::string& full_path) {
        auto it = table.table.find(key);
        if (it != table.table.end())
            fail("duplicate key '" + full_path + "'");
        table.table.emplace(key, std::move(value));
    }

    Value& parseTableHeader(Value& root) {
        int header_line = line_;
        take();  // '['
        if (peek() == '[')
            failAt(header_line,
                   "array-of-tables headers [[...]] are not supported "
                   "by the Parlay config schema");
        skipInlineSpace();
        std::vector<std::string> parts = parseKey();
        skipInlineSpace();
        if (peek() != ']')
            failAt(header_line, "expected ']' to close table header");
        take();  // ']'
        expectEol("table header");

        // Walk/create the table. Only the final segment counts as
        // defined by this header: super-tables created implicitly by
        // [a.b.c] (a, a.b) may still be defined later by their own
        // header, but [a.b.c] twice is a duplicate.
        Value* cur = &root;
        std::string path;
        for (size_t i = 0; i < parts.size(); ++i) {
            const std::string& part = parts[i];
            bool last = (i + 1 == parts.size());
            if (!path.empty()) path += ".";
            path += part;
            auto it = cur->table.find(part);
            if (it == cur->table.end()) {
                auto inserted =
                    cur->table.emplace(part, Value::makeTable({}));
                cur = &inserted.first->second;
                defined_tables_[path] = last;
            } else {
                if (!it->second.isTable())
                    failAt(header_line,
                           "table '" + path +
                               "' is already defined as a value");
                auto dit = defined_tables_.find(path);
                if (last && dit != defined_tables_.end() && dit->second)
                    failAt(header_line,
                           "duplicate table definition [" + path + "]");
                if (last) defined_tables_[path] = true;
                cur = &it->second;
            }
        }
        section_path_ = path;
        return *cur;
    }

    void parseKeyValue(Value& table) {
        int kv_line = line_;
        std::vector<std::string> parts = parseKey();
        skipInlineSpace();
        if (peek() != '=')
            failAt(kv_line, "expected '=' after key");
        take();  // '='
        skipInlineSpace();
        Value value = parseValue();
        // The value parser stops at the first character that is not
        // part of the value; a second value on the line is garbage.
        expectEol("value");

        std::string full;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) full += ".";
            full += parts[i];
        }
        Value* target = &table;
        if (parts.size() > 1) {
            // Dotted key: intermediate tables are created implicitly
            // under the current section and recorded (full path) so a
            // later [header] for one of them is a duplicate.
            target = navigate(table, parts, section_path_, defined_tables_);
        }
        insertValue(*target, parts.back(), std::move(value), full);
    }

    // --- strings -------------------------------------------------------
    std::string parseLiteralString() {
        take();  // '\''
        if (peek() == '\'' && peek(1) == '\'') {
            take();
            take();
            if (peek() == '\r' && peek(1) == '\n') {
                take();
                take();
            } else if (peek() == '\n') {
                take();
            }
            std::string out;
            for (;;) {
                if (eof()) fail("unterminated multiline literal string");
                if (peek() == '\'' && peek(1) == '\'' &&
                    peek(2) == '\'') {
                    take();
                    take();
                    take();
                    break;
                }
                out += take();
            }
            return out;
        }
        std::string out;
        for (;;) {
            if (eof() || peek() == '\n')
                fail("unterminated literal string");
            char c = take();
            if (c == '\'') break;
            out += c;
        }
        return out;
    }

    // --- values --------------------------------------------------------
    Value parseValue() {
        char c = peek();
        if (c == '"') {
            return Value::makeString(parseBasicStringUtf8());
        }
        if (c == '\'') return Value::makeString(parseLiteralString());
        if (c == '[') return parseArray();
        if (c == '{') return parseInlineTable();
        if (c == 't' || c == 'f') return parseBool();
        return parseNumberOrDatetime();
    }

    // Basic string with full UTF-8 \u/\U support.
    std::string parseBasicStringUtf8() { return parseBasicStringFull(); }

    std::string parseBasicStringFull() {
        take();  // '"'
        if (peek() == '"' && peek(1) == '"') {
            take();
            take();
            return parseMultilineBasicStringFull();
        }
        std::string out;
        for (;;) {
            if (eof() || peek() == '\n')
                fail("unterminated string");
            char c = take();
            if (c == '"') break;
            if (c == '\\') {
                char e = peek();
                if (e == 'u' || e == 'U') {
                    take();
                    out += encodeUtf8(parseHex(e == 'u' ? 4 : 8));
                } else {
                    out += parseEscapeChar(e);
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    std::string parseMultilineBasicStringFull() {
        if (peek() == '\r' && peek(1) == '\n') {
            take();
            take();
        } else if (peek() == '\n') {
            take();
        }
        std::string out;
        for (;;) {
            if (eof()) fail("unterminated multiline string");
            if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
                take();
                take();
                take();
                if (peek() == '"') out += take();
                if (peek() == '"') out += take();
                break;
            }
            char c = take();
            if (c == '\\') {
                size_t save_pos = pos_;
                int save_line = line_;
                skipInlineSpace();
                if (peek() == '\n' ||
                    (peek() == '\r' && peek(1) == '\n')) {
                    while (!eof() &&
                           (peek() == ' ' || peek() == '\t' ||
                            peek() == '\n' || peek() == '\r'))
                        take();
                } else {
                    pos_ = save_pos;
                    line_ = save_line;
                    char e = peek();
                    if (e == 'u' || e == 'U') {
                        take();
                        out += encodeUtf8(parseHex(e == 'u' ? 4 : 8));
                    } else {
                        out += parseEscapeChar(e);
                    }
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    // Consumes the escape letter (peek) and returns the decoded char
    // for the single-byte escapes; \u/\U are handled by the caller.
    char parseEscapeChar(char e) {
        take();  // the escape letter
        switch (e) {
            case 'b': return '\b';
            case 't': return '\t';
            case 'n': return '\n';
            case 'f': return '\f';
            case 'r': return '\r';
            case '"': return '"';
            case '\\': return '\\';
            default:
                fail(std::string("invalid escape sequence '\\") + e +
                     "' in string");
        }
    }

    uint32_t parseHex(int digits) {
        uint32_t v = 0;
        for (int i = 0; i < digits; ++i) {
            char c = take();
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("invalid hex digit in \\u escape");
        }
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
            fail("invalid Unicode scalar value in \\u escape");
        return v;
    }

    static std::string encodeUtf8(uint32_t cp) {
        std::string out;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return out;
    }

    Value parseBool() {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return Value::makeBoolean(true);
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return Value::makeBoolean(false);
        }
        fail("invalid value: expected true/false, a number, a string, "
             "an array, or an inline table");
    }

    Value parseArray() {
        take();  // '['
        std::vector<Value> items;
        for (;;) {
            skipSpaceAndComments(/*newlines_ok=*/true);
            if (eof()) fail("unterminated array");
            if (peek() == ']') {
                take();
                return Value::makeArray(std::move(items));
            }
            items.push_back(parseValue());
            skipSpaceAndComments(/*newlines_ok=*/true);
            if (peek() == ',') {
                take();
                continue;
            }
            if (peek() == ']') {
                take();
                return Value::makeArray(std::move(items));
            }
            fail("expected ',' or ']' in array");
        }
    }

    Value parseInlineTable() {
        take();  // '{'
        Value table = Value::makeTable({});
        skipInlineSpace();
        if (peek() == '}') {
            take();
            return table;
        }
        for (;;) {
            skipInlineSpace();
            std::vector<std::string> parts = parseKey();
            skipInlineSpace();
            if (peek() != '=')
                fail("expected '=' in inline table");
            take();
            skipInlineSpace();
            Value value = parseValue();
            // Inline tables are single-line in TOML 1.0 (no newlines
            // inside); a newline here is therefore an error, caught
            // by the ',' / '}' check below.
            Value* target = &table;
            std::map<std::string, bool> dummy;
            if (parts.size() > 1)
                target = navigate(table, parts, "", dummy);
            std::string full;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) full += ".";
                full += parts[i];
            }
            insertValue(*target, parts.back(), std::move(value), full);
            skipInlineSpace();
            if (peek() == ',') {
                take();
                continue;
            }
            if (peek() == '}') {
                take();
                return table;
            }
            fail("expected ',' or '}' in inline table");
        }
    }

    Value parseNumberOrDatetime() {
        size_t start = pos_;
        int start_line = line_;
        // inf / nan (case-sensitive, per TOML).
        if (text_.compare(pos_, 3, "inf") == 0) {
            pos_ += 3;
            return Value::makeFloat(HUGE_VAL);
        }
        if (text_.compare(pos_, 4, "+inf") == 0) {
            pos_ += 4;
            return Value::makeFloat(HUGE_VAL);
        }
        if (text_.compare(pos_, 4, "-inf") == 0) {
            pos_ += 4;
            return Value::makeFloat(-HUGE_VAL);
        }
        if (text_.compare(pos_, 3, "nan") == 0) {
            pos_ += 3;
            return Value::makeFloat(NAN);
        }
        if (text_.compare(pos_, 4, "+nan") == 0) {
            pos_ += 4;
            return Value::makeFloat(NAN);
        }
        if (text_.compare(pos_, 4, "-nan") == 0) {
            pos_ += 4;
            return Value::makeFloat(NAN);
        }

        // Scan the numeric token: digits, sign, '.', 'e/E', 'x/o/b',
        // '_', ':', '-' (dates use '-' and ':'). A token containing
        // '-' past the leading sign or ':' is a datetime, which the
        // schema does not use.
        std::string tok;
        if (peek() == '+' || peek() == '-') tok += take();
        bool saw_digit = false;
        bool saw_dot = false;
        bool saw_exp = false;
        bool saw_colon = false;
        bool saw_date_dash = false;
        while (!eof()) {
            char c = peek();
            if (c >= '0' && c <= '9') {
                saw_digit = true;
                tok += take();
            } else if (c == '_') {
                tok += take();
            } else if (c == '.' && !saw_dot && !saw_exp) {
                saw_dot = true;
                tok += take();
            } else if ((c == 'e' || c == 'E') && saw_digit && !saw_exp) {
                saw_exp = true;
                tok += take();
                if (peek() == '+' || peek() == '-') tok += take();
            } else if ((c == 'x' || c == 'X' || c == 'o' || c == 'O' ||
                        c == 'b' || c == 'B') &&
                       tok.size() >= 1 && tok.back() == '0' && !saw_dot &&
                       !saw_exp) {
                tok += take();
                // Hex/oct/bin digits (and underscores) follow.
                while (!eof()) {
                    char d = peek();
                    if ((d >= '0' && d <= '9') ||
                        (d >= 'a' && d <= 'f') ||
                        (d >= 'A' && d <= 'F') || d == '_')
                        tok += take();
                    else
                        break;
                }
                break;
            } else if (c == ':' ) {
                saw_colon = true;
                tok += take();
            } else if (c == '-' && saw_digit) {
                saw_date_dash = true;
                tok += take();
            } else if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                // 'a'..'f' inside a bare word (e.g. a mistyped value):
                // consume so the error names the whole token.
                tok += take();
            } else {
                break;
            }
        }
        if (saw_colon || saw_date_dash)
            failAt(start_line,
                   "datetime values are not supported by the Parlay "
                   "config schema (found '" +
                       tok + "')");
        if (!saw_digit)
            failAt(start_line, "invalid value '" + tok +
                                   "': expected a number, string, "
                                   "boolean, array, or inline table");
        return parseNumberToken(tok, start_line, start);
    }

    Value parseNumberToken(const std::string& tok, int tok_line,
                           size_t tok_start) {
        (void)tok_start;
        // Underscore rules: no leading/trailing underscore, no doubles,
        // none adjacent to '.', 'e', 'x', or a sign.
        for (size_t i = 0; i < tok.size(); ++i) {
            if (tok[i] != '_') continue;
            if (i == 0 || i + 1 >= tok.size())
                failAt(tok_line, "invalid number '" + tok +
                                     "': misplaced underscore");
            char prev = tok[i - 1], next = tok[i + 1];
            auto is_digit = [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            };
            if (!is_digit(prev) || !is_digit(next))
                failAt(tok_line, "invalid number '" + tok +
                                     "': underscore must sit between "
                                     "digits");
        }
        std::string clean;
        for (char c : tok)
            if (c != '_') clean += c;

        // Hex / octal / binary.
        size_t body = (clean[0] == '+' || clean[0] == '-') ? 1 : 0;
        if (clean.size() - body >= 2 && clean[body] == '0' &&
            (clean[body + 1] == 'x' || clean[body + 1] == 'X' ||
             clean[body + 1] == 'o' || clean[body + 1] == 'O' ||
             clean[body + 1] == 'b' || clean[body + 1] == 'B')) {
            int base = clean[body + 1] == 'x' || clean[body + 1] == 'X'
                           ? 16
                           : (clean[body + 1] == 'o' ||
                                      clean[body + 1] == 'O'
                                  ? 8
                                  : 2);
            std::string digits = clean.substr(body + 2);
            if (digits.empty())
                failAt(tok_line,
                       "invalid number '" + tok + "': missing digits");
            try {
                size_t used = 0;
                long long v = std::stoll(digits, &used, base);
                if (used != digits.size())
                    failAt(tok_line, "invalid number '" + tok + "'");
                if (clean[0] == '-') v = -v;
                return Value::makeInteger(static_cast<int64_t>(v));
            } catch (const ParseError&) {
                throw;
            } catch (...) {
                failAt(tok_line, "integer '" + tok + "' is out of range");
            }
        }

        bool is_float = clean.find_first_of(".eE") != std::string::npos;
        try {
            if (is_float) {
                size_t used = 0;
                double v = std::stod(clean, &used);
                if (used != clean.size())
                    failAt(tok_line, "invalid number '" + tok + "'");
                return Value::makeFloat(v);
            }
            // Decimal integer: no leading zeros (TOML rule).
            std::string digits =
                clean.substr(body, std::string::npos);
            if (digits.size() > 1 && digits[0] == '0')
                failAt(tok_line, "invalid integer '" + tok +
                                     "': leading zeros are not allowed");
            size_t used = 0;
            long long v = std::stoll(clean, &used, 10);
            if (used != clean.size())
                failAt(tok_line, "invalid number '" + tok + "'");
            return Value::makeInteger(static_cast<int64_t>(v));
        } catch (const ParseError&) {
            throw;
        } catch (...) {
            failAt(tok_line, "number '" + tok + "' is out of range");
        }
    }

    // Tables defined by an explicit [header] or by a dotted key
    // (root-relative path -> true means "defined, may not be defined
    // again"; false means "implicit super-table, a header may define
    // it later").
    std::map<std::string, bool> defined_tables_;
    // Root-relative path of the [table] section currently receiving
    // keys ("" at the document root).
    std::string section_path_;
};

}  // namespace

Document parse(const std::string& text, const std::string& source_name) {
    Parser p(text, source_name);
    return p.run();
}

Document parseFile(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("toml: cannot read config file '" + path +
                                 "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
        throw std::runtime_error("toml: failed reading config file '" +
                                 path + "'");
    return parse(ss.str(), path);
}

}  // namespace parlay::toml
