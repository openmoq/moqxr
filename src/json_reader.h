#pragma once

// Minimal JSON reader, extracted from live_srt_config.cpp so more than one
// config parser can use it. Internal to the library: not part of the public
// include/ API.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace openmoq::publisher::internal {

struct JsonValue;
using JsonObject = std::unordered_map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

// Use unique_ptr wrappers for recursive types to avoid incomplete-type errors on GCC 11.
struct JsonValue {
    using Value = std::variant<std::nullptr_t, bool, double, std::string,
                               std::unique_ptr<JsonArray>, std::unique_ptr<JsonObject>>;
    Value value;

    // Convenience constructors so existing code using JsonValue{...} still works.
    JsonValue() : value(nullptr) {}
    JsonValue(std::nullptr_t) : value(nullptr) {}
    JsonValue(bool b) : value(b) {}
    JsonValue(double d) : value(d) {}
    JsonValue(std::string s) : value(std::move(s)) {}
    JsonValue(JsonArray arr) : value(std::make_unique<JsonArray>(std::move(arr))) {}
    JsonValue(JsonObject obj) : value(std::make_unique<JsonObject>(std::move(obj))) {}

    // Copy support (needed for vector/map operations)
    JsonValue(const JsonValue& other);
    JsonValue& operator=(const JsonValue& other);
    JsonValue(JsonValue&&) = default;
    JsonValue& operator=(JsonValue&&) = default;
    ~JsonValue() = default;
};

inline JsonValue::JsonValue(const JsonValue& other) {
    struct CopyVisitor {
        JsonValue::Value operator()(std::nullptr_t) const { return nullptr; }
        JsonValue::Value operator()(bool b) const { return b; }
        JsonValue::Value operator()(double d) const { return d; }
        JsonValue::Value operator()(const std::string& s) const { return s; }
        JsonValue::Value operator()(const std::unique_ptr<JsonArray>& a) const {
            return a ? std::make_unique<JsonArray>(*a) : std::make_unique<JsonArray>();
        }
        JsonValue::Value operator()(const std::unique_ptr<JsonObject>& o) const {
            return o ? std::make_unique<JsonObject>(*o) : std::make_unique<JsonObject>();
        }
    };
    value = std::visit(CopyVisitor{}, other.value);
}

inline JsonValue& JsonValue::operator=(const JsonValue& other) {
    if (this != &other) {
        JsonValue tmp(other);
        value = std::move(tmp.value);
    }
    return *this;
}

class JsonParser {
public:
    explicit JsonParser(std::string input)
        : input_(std::move(input)) {}

    JsonValue parse() {
        skip_ws();
        JsonValue root = parse_value();
        skip_ws();
        if (!eof()) {
            throw std::runtime_error("unexpected trailing JSON content");
        }
        return root;
    }

private:
    JsonValue parse_value() {
        if (eof()) {
            throw std::runtime_error("unexpected end of JSON input");
        }

        const char ch = peek();
        if (ch == '{') {
            return JsonValue{parse_object()};
        }
        if (ch == '[') {
            return JsonValue{parse_array()};
        }
        if (ch == '"') {
            return JsonValue{parse_string()};
        }
        if (ch == 't') {
            consume_literal("true");
            return JsonValue{true};
        }
        if (ch == 'f') {
            consume_literal("false");
            return JsonValue{false};
        }
        if (ch == 'n') {
            consume_literal("null");
            return JsonValue{nullptr};
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return JsonValue{parse_number()};
        }
        throw std::runtime_error("unsupported JSON token");
    }

    JsonObject parse_object() {
        expect('{');
        skip_ws();
        JsonObject object;
        if (consume_if('}')) {
            return object;
        }

        while (true) {
            skip_ws();
            const std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            object.emplace(key, parse_value());
            skip_ws();
            if (consume_if('}')) {
                break;
            }
            expect(',');
        }
        return object;
    }

    JsonArray parse_array() {
        expect('[');
        skip_ws();
        JsonArray array;
        if (consume_if(']')) {
            return array;
        }

        while (true) {
            skip_ws();
            array.push_back(parse_value());
            skip_ws();
            if (consume_if(']')) {
                break;
            }
            expect(',');
        }
        return array;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (!eof()) {
            const char ch = take();
            if (ch == '"') {
                return out;
            }
            if (ch == '\\') {
                if (eof()) {
                    throw std::runtime_error("unterminated JSON escape");
                }
                const char esc = take();
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default:
                        throw std::runtime_error("unsupported JSON escape sequence");
                }
                continue;
            }
            out.push_back(ch);
        }
        throw std::runtime_error("unterminated JSON string");
    }

    double parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            take();
        }
        if (eof()) {
            throw std::runtime_error("invalid JSON number");
        }
        if (peek() == '0') {
            take();
        } else {
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                take();
            }
        }
        if (!eof() && peek() == '.') {
            take();
            if (eof() || std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                throw std::runtime_error("invalid JSON number fraction");
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                take();
            }
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            take();
            if (!eof() && (peek() == '+' || peek() == '-')) {
                take();
            }
            if (eof() || std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                throw std::runtime_error("invalid JSON exponent");
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                take();
            }
        }

        return std::stod(input_.substr(start, pos_ - start));
    }

    void consume_literal(std::string_view literal) {
        for (const char ch : literal) {
            if (eof() || take() != ch) {
                throw std::runtime_error("invalid JSON literal");
            }
        }
    }

    void skip_ws() {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
            ++pos_;
        }
    }

    void expect(char ch) {
        if (eof() || take() != ch) {
            throw std::runtime_error("unexpected JSON token");
        }
    }

    bool consume_if(char ch) {
        if (!eof() && peek() == ch) {
            ++pos_;
            return true;
        }
        return false;
    }

    char peek() const {
        return input_[pos_];
    }

    char take() {
        return input_[pos_++];
    }

    bool eof() const {
        return pos_ >= input_.size();
    }

    std::string input_;
    std::size_t pos_ = 0;
};

inline const JsonObject& expect_object(const JsonValue& value, std::string_view field_name) {
    const auto* ptr = std::get_if<std::unique_ptr<JsonObject>>(&value.value);
    if (ptr == nullptr || *ptr == nullptr) {
        throw std::runtime_error(std::string(field_name) + " must be an object");
    }
    return **ptr;
}

inline const JsonArray& expect_array(const JsonValue& value, std::string_view field_name) {
    const auto* ptr = std::get_if<std::unique_ptr<JsonArray>>(&value.value);
    if (ptr == nullptr || *ptr == nullptr) {
        throw std::runtime_error(std::string(field_name) + " must be an array");
    }
    return **ptr;
}

inline std::string expect_string(const JsonObject& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        throw std::runtime_error("missing JSON key: " + std::string(key));
    }
    const auto* str = std::get_if<std::string>(&it->second.value);
    if (str == nullptr) {
        throw std::runtime_error("JSON key must be a string: " + std::string(key));
    }
    return *str;
}

inline bool read_bool_or_default(const JsonObject& object, std::string_view key, bool default_value) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return default_value;
    }
    const auto* value = std::get_if<bool>(&it->second.value);
    if (value == nullptr) {
        throw std::runtime_error("JSON key must be a bool: " + std::string(key));
    }
    return *value;
}

inline std::optional<std::uint32_t> read_optional_u32(const JsonObject& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) {
        return std::nullopt;
    }
    if (std::holds_alternative<std::nullptr_t>(it->second.value)) {
        return std::nullopt;
    }
    const auto* number = std::get_if<double>(&it->second.value);
    if (number == nullptr) {
        throw std::runtime_error("JSON key must be a number or null: " + std::string(key));
    }
    if (*number < 0.0 || *number > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("JSON key out of range: " + std::string(key));
    }
    return static_cast<std::uint32_t>(*number);
}

inline std::uint32_t read_u32_or_default(const JsonObject& object, std::string_view key, std::uint32_t default_value) {
    const auto value = read_optional_u32(object, key);
    return value.has_value() ? *value : default_value;
}

}  // namespace openmoq::publisher::internal
