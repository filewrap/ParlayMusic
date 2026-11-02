// json.cpp — recursive-descent parser + serializer for parlay::Json.
#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace parlay {
namespace {

struct Parser {
    const std::string& s;
    size_t pos = 0;

    explicit Parser(const std::string& text) : s(text) {}

    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error("JSON parse error at offset " +
                                 std::to_string(pos) + ": " + msg);
    }
    void skipWs() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                                  s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }
    char peek() {
        skipWs();
        if (pos >= s.size()) fail("unexpected end of input");
        return s[pos];
    }
    char take() {
        skipWs();
        if (pos >= s.size()) fail("unexpected end of input");
        return s[pos++];
    }
    void expect(char c) {
        if (take() != c) fail(std::string("expected '") + c + "'");
    }

    Json parseValue() {
        char c = peek();
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': return Json(parseString());
            case 't': parseLiteral("true"); return Json(true);
            case 'f': parseLiteral("false"); return Json(false);
            case 'n': parseLiteral("null"); return Json(nullptr);
            default: return parseNumber();
        }
    }

    void parseLiteral(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (pos >= s.size() || s[pos++] != *p) fail("bad literal");
        }
    }

    Json parseObject() {
        expect('{');
        Json::Object obj;
        if (peek() == '}') {
            ++pos;
            return Json(std::move(obj));
        }
        while (true) {
            if (peek() != '"') fail("expected object key");
            std::string key = parseString();
            expect(':');
            obj[std::move(key)] = parseValue();
            char c = take();
            if (c == '}') break;
            if (c != ',') fail("expected ',' or '}' in object");
        }
        return Json(std::move(obj));
    }

    Json parseArray() {
        expect('[');
        Json::Array arr;
        if (peek() == ']') {
            ++pos;
            return Json(std::move(arr));
        }
        while (true) {
            arr.push_back(parseValue());
            char c = take();
            if (c == ']') break;
            if (c != ',') fail("expected ',' or ']' in array");
        }
        return Json(std::move(arr));
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    uint32_t parseHex4() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos >= s.size()) fail("truncated \\u escape");
            char c = s[pos++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("bad hex digit in \\u escape");
        }
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (pos >= s.size()) fail("unterminated string");
            char c = s[pos++];
            if (c == '"') break;
            if (c == '\\') {
                if (pos >= s.size()) fail("unterminated escape");
                char e = s[pos++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = parseHex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            pos + 1 < s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                            pos += 2;
                            uint32_t lo = parseHex4();
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Json parseNumber() {
        size_t start = pos;
        if (pos < s.size() && s[pos] == '-') ++pos;
        bool isDouble = false;
        while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) ||
                                  s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' ||
                                  s[pos] == '+' || s[pos] == '-')) {
            if (s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E') isDouble = true;
            ++pos;
        }
        if (pos == start) fail("expected value");
        std::string num = s.substr(start, pos - start);
        try {
            if (isDouble) return Json(std::stod(num));
            return Json(static_cast<int64_t>(std::stoll(num)));
        } catch (...) {
            fail("bad number");
        }
    }
};

void dumpValue(const Json& j, std::string& out);

void dumpString(const std::string& s, std::string& out) {
    out += Json::escape(s);
}

void dumpValue(const Json& j, std::string& out) {
    if (j.isNull()) {
        out += "null";
    } else if (j.isBool()) {
        out += j.asBool() ? "true" : "false";
    } else if (j.isInt()) {
        out += std::to_string(j.asInt());
    } else if (j.isString()) {
        dumpString(j.asString(), out);
    } else if (j.isArray()) {
        out += '[';
        bool first = true;
        for (const auto& el : j.asArray()) {
            if (!first) out += ',';
            first = false;
            dumpValue(el, out);
        }
        out += ']';
    } else if (j.isObject()) {
        out += '{';
        bool first = true;
        for (const auto& kv : j.asObject()) {
            if (!first) out += ',';
            first = false;
            dumpString(kv.first, out);
            out += ':';
            dumpValue(kv.second, out);
        }
        out += '}';
    } else {  // double
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", j.asDouble());
        out += buf;
    }
}

}  // namespace

Json Json::parse(const std::string& text) {
    Parser p(text);
    Json v = p.parseValue();
    p.skipWs();
    if (p.pos != text.size())
        throw std::runtime_error("JSON parse error: trailing characters");
    return v;
}

std::string Json::dump() const {
    std::string out;
    dumpValue(*this, out);
    return out;
}

std::string Json::escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace parlay
