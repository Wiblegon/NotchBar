#pragma once
// Minimal JSON value + parser/writer (UTF-8). Only what settings/favorites/caches need.
// Kept in-tree instead of pulling a third-party library to keep dependencies at zero.
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nb::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    Value(int n) : v_(static_cast<double>(n)) {}
    Value(int64_t n) : v_(static_cast<double>(n)) {}
    Value(double n) : v_(n) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? std::get<bool>(v_) : def; }
    double asNumber(double def = 0) const { return isNumber() ? std::get<double>(v_) : def; }
    int asInt(int def = 0) const { return isNumber() ? static_cast<int>(std::get<double>(v_)) : def; }
    std::string asString(const std::string& def = {}) const { return isString() ? std::get<std::string>(v_) : def; }
    const Array& asArray() const;
    const Object& asObject() const;
    Array& mutArray();
    Object& mutObject();

    // Object access; returns a shared null for missing keys.
    const Value& operator[](const std::string& key) const;
    Value& set(const std::string& key, Value v);
    bool has(const std::string& key) const;

private:
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_{nullptr};
};

struct ParseError {
    size_t offset = 0;
    std::string message;
};

// Returns nullopt on malformed input (error filled when provided). Never throws.
std::optional<Value> Parse(std::string_view text, ParseError* error = nullptr);
std::string Serialize(const Value& v, bool pretty = true);

} // namespace nb::json
