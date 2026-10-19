#pragma once

// A small, strict JSON (RFC 8259) parser and writer for the lobby and the control
// channel (docs/decisions/0004-http-and-json.md). Input is hostile: the parser has
// hard limits on size, depth, value count and string length, rejects anything the
// RFC does not allow (comments, trailing commas, leading zeros, bare control
// characters, invalid UTF-8, lone surrogates, duplicate object keys) and NUL
// characters, and never recurses deeper than the depth limit.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace npk::json {

struct Limits {
  std::size_t max_bytes = 64 * 1024;
  std::size_t max_depth = 16;
  std::size_t max_values = 4096;   // all values in the document, containers included
  std::size_t max_string = 4096;   // bytes of one string or key, decoded
};

class Value {
 public:
  using Array = std::vector<Value>;
  using Object = std::vector<std::pair<std::string, Value>>;  // in document order

  Value() = default;                          // null
  Value(std::nullptr_t) {}
  Value(bool b) : v_(b) {}
  Value(int i) : v_(static_cast<std::int64_t>(i)) {}
  Value(std::int64_t i) : v_(i) {}
  Value(std::uint32_t i) : v_(static_cast<std::int64_t>(i)) {}
  Value(double d) : v_(d) {}
  Value(const char* s) : v_(std::string(s)) {}
  Value(std::string s) : v_(std::move(s)) {}
  Value(std::string_view s) : v_(std::string(s)) {}
  Value(Array a) : v_(std::move(a)) {}
  Value(Object o) : v_(std::move(o)) {}

  bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
  bool is_bool() const { return std::holds_alternative<bool>(v_); }
  bool is_int() const { return std::holds_alternative<std::int64_t>(v_); }
  bool is_number() const { return is_int() || std::holds_alternative<double>(v_); }
  bool is_string() const { return std::holds_alternative<std::string>(v_); }
  bool is_array() const { return std::holds_alternative<Array>(v_); }
  bool is_object() const { return std::holds_alternative<Object>(v_); }

  // Each returns nothing when the value is of another type.
  std::optional<bool> as_bool() const;
  std::optional<std::int64_t> as_int() const;   // an integer, or a double that is one exactly
  std::optional<double> as_double() const;
  const std::string* as_string() const;
  const Array* as_array() const;
  const Object* as_object() const;

  // An object's member, or null when this is not an object or has no such key.
  const Value* find(std::string_view key) const;
  // Appends to an array / sets on an object (replacing a key); a null value becomes the container.
  Value& push_back(Value v);
  Value& set(std::string key, Value v);

 private:
  std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> v_;
};

// On failure returns nothing and, if given, says why in *error.
std::optional<Value> parse(std::string_view text, const Limits& limits = {}, std::string* error = nullptr);

// Compact output. Non-finite doubles are written as null.
std::string dump(const Value& v);

}  // namespace npk::json
