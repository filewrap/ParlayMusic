// toml.hpp — strict, in-tree TOML parser (TOML v1.0 subset).
//
// There is no toml++ (or any TOML library) in the sandbox, so Parlay
// carries its own parser. It parses exactly what the unified config
// schema (parlay.toml, FINAL-ARCHITECTURE.md §1.4) uses, and nothing
// is silently defaulted:
//
//   - tables [a] and [a.b], dotted keys (a.b = 1)
//   - basic strings "..." (escapes, multiline """...""") and literal
//     strings '...' (multiline '''...''')
//   - integers (decimal, 0x/0o/0b, underscores, +/-), floats
//     (exponents, inf/nan parsed then rejected as non-finite by the
//     caller where a finite value is required)
//   - booleans, arrays of values (multiline, trailing comma, comments)
//   - inline tables { k = v, ... }
//   - comments (# ...) to end of line
//
// Strictness: duplicate keys/tables, malformed numbers, unterminated
// strings/arrays, bare keys with illegal characters, values after a
// value on the same line, and [[array-of-tables]] headers are all
// hard errors carrying the exact line number and reason — never a
// silent default. Datetimes are parsed as an error (unsupported value
// type): the config schema has no datetime field, and silently
// accepting one would hide a typo.
//
// No I/O inside the parser except parseFile(); no secrets involved.
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace parlay::toml {

// Thrown on any malformed input. `line` is the 1-based line where the
// problem was found; what() is "toml: <source>:<line>: <detail>".
class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& source, int line, const std::string& detail);
    int line() const { return line_; }
    const std::string& source() const { return source_; }

private:
    std::string source_;
    int line_;
};

struct Value {
    enum class Type { String, Integer, Float, Boolean, Array, Table };

    Type type = Type::String;
    std::string str;              // String
    int64_t integer = 0;          // Integer
    double floating = 0.0;        // Float
    bool boolean = false;         // Boolean
    std::vector<Value> array;     // Array
    std::map<std::string, Value> table;  // Table

    static Value makeString(std::string v);
    static Value makeInteger(int64_t v);
    static Value makeFloat(double v);
    static Value makeBoolean(bool v);
    static Value makeArray(std::vector<Value> v);
    static Value makeTable(std::map<std::string, Value> v);

    bool isString() const { return type == Type::String; }
    bool isInteger() const { return type == Type::Integer; }
    bool isFloat() const { return type == Type::Float; }
    bool isBoolean() const { return type == Type::Boolean; }
    bool isArray() const { return type == Type::Array; }
    bool isTable() const { return type == Type::Table; }
    // Human-readable type name, for error messages.
    std::string typeName() const;
};

// A parsed TOML document: the root table. Lookups are by dotted path
// ("rate_limit.chat.capacity"); find() returns nullptr when any
// segment is absent or is not a table (except the final segment).
class Document {
public:
    Document() : root_(Value::makeTable({})) {}
    explicit Document(Value root) : root_(std::move(root)) {}

    const Value& root() const { return root_; }
    const Value* find(const std::string& dotted_path) const;

    // Typed accessors. Each throws ParseError (with the document's
    // source name and line 0 — the key, not a file position, is the
    // useful context here) when the key is absent or the wrong type.
    // The caller (config.cpp) re-wraps these as ConfigError with the
    // config key name.
    std::string getString(const std::string& path) const;
    int64_t getInteger(const std::string& path) const;
    double getNumber(const std::string& path) const;  // int or float
    bool getBoolean(const std::string& path) const;
    std::vector<std::string> getStringArray(const std::string& path) const;
    const Value* getTable(const std::string& path) const;
    bool has(const std::string& path) const { return find(path) != nullptr; }

private:
    Value root_;
};

// Parse TOML text. `source_name` appears in error messages (a file
// path or a label such as "<parlay.toml.example>").
Document parse(const std::string& text, const std::string& source_name);

// Read a file and parse it. Throws ParseError on malformed input and
// std::runtime_error (message includes the path) when the file cannot
// be read.
Document parseFile(const std::string& path);

}  // namespace parlay::toml
