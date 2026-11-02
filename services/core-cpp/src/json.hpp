// json.hpp — minimal JSON value + parser + serializer.
// Hand-rolled: no nlohmann/json in this sandbox and no network fetch of
// deps is allowed. Covers everything the Telegram Bot API and the Parlay
// IPC contract (contracts/ipc.json) return: objects, arrays, strings
// (with \u escapes), integers, doubles, booleans, null.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace parlay {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<std::monostate, bool, int64_t, double,
                               std::string, Array, Object>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : value_(v) {}
    Json(int v) : value_(static_cast<int64_t>(v)) {}
    Json(int64_t v) : value_(v) {}
    Json(double v) : value_(v) {}
    Json(const char* v) : value_(std::string(v)) {}
    Json(std::string v) : value_(std::move(v)) {}
    Json(Array v) : value_(std::move(v)) {}
    Json(Object v) : value_(std::move(v)) {}

    static Json parse(const std::string& text);
    std::string dump() const;

    bool isNull() const { return std::holds_alternative<std::monostate>(value_); }
    bool isBool() const { return std::holds_alternative<bool>(value_); }
    bool isInt() const { return std::holds_alternative<int64_t>(value_); }
    bool isNumber() const { return isInt() || std::holds_alternative<double>(value_); }
    bool isString() const { return std::holds_alternative<std::string>(value_); }
    bool isArray() const { return std::holds_alternative<Array>(value_); }
    bool isObject() const { return std::holds_alternative<Object>(value_); }

    bool asBool(bool fallback = false) const {
        return isBool() ? std::get<bool>(value_) : fallback;
    }
    int64_t asInt(int64_t fallback = 0) const {
        if (isInt()) return std::get<int64_t>(value_);
        if (std::holds_alternative<double>(value_))
            return static_cast<int64_t>(std::get<double>(value_));
        return fallback;
    }
    double asDouble(double fallback = 0.0) const {
        if (std::holds_alternative<double>(value_)) return std::get<double>(value_);
        if (isInt()) return static_cast<double>(std::get<int64_t>(value_));
        return fallback;
    }
    const std::string& asString() const {
        static const std::string kEmpty;
        return isString() ? std::get<std::string>(value_) : kEmpty;
    }
    const Array& asArray() const {
        static const Array kEmpty;
        return isArray() ? std::get<Array>(value_) : kEmpty;
    }
    const Object& asObject() const {
        static const Object kEmpty;
        return isObject() ? std::get<Object>(value_) : kEmpty;
    }

    // Object lookup: nullptr when absent or not an object.
    const Json* find(const std::string& key) const {
        if (!isObject()) return nullptr;
        const auto& obj = std::get<Object>(value_);
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    // Array element: nullptr when out of range or not an array.
    const Json* at(size_t i) const {
        if (!isArray()) return nullptr;
        const auto& arr = std::get<Array>(value_);
        return i < arr.size() ? &arr[i] : nullptr;
    }
    size_t size() const {
        if (isArray()) return std::get<Array>(value_).size();
        if (isObject()) return std::get<Object>(value_).size();
        return 0;
    }

    void set(const std::string& key, Json v) {
        if (!isObject()) value_ = Object{};
        std::get<Object>(value_)[key] = std::move(v);
    }
    void push(Json v) {
        if (!isArray()) value_ = Array{};
        std::get<Array>(value_).push_back(std::move(v));
    }

    // Build {"k": "v", ...} from pairs, and escape a raw string for JSON.
    static Json objectOf(std::initializer_list<std::pair<const char*, Json>> items) {
        Object obj;
        for (auto& kv : items) obj[kv.first] = kv.second;
        return Json(std::move(obj));
    }
    static std::string escape(const std::string& s);

private:
    Value value_;
};

}  // namespace parlay
