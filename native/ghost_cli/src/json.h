// A small JSON value type: enough to parse a request and build a response.
//
// The control plane speaks JSON over a named pipe. Pulling in a third-party
// library for this would be the only external dependency in the whole product,
// and the surface actually needed is tiny — objects, arrays, strings, numbers,
// booleans — so it is written out here instead.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ghost {

class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Json() = default;
  static Json boolean(bool value);
  static Json number(double value);
  static Json integer(long long value);
  static Json string(const std::string& value);
  static Json array();
  static Json object();

  // Returns a null value and fills `error` on malformed input.
  static Json parse(const std::string& text, std::string* error);

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }
  bool is_object() const { return type_ == Type::kObject; }
  bool is_array() const { return type_ == Type::kArray; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_number() const { return type_ == Type::kNumber; }

  bool as_bool(bool fallback = false) const;
  double as_number(double fallback = 0.0) const;
  std::string as_string(const std::string& fallback = std::string()) const;

  // Object access. `find` returns nullptr when the key is absent.
  const Json* find(const std::string& key) const;
  void set(const std::string& key, Json value);
  const std::vector<std::pair<std::string, Json>>& fields() const { return fields_; }

  // Array access.
  void push(Json value);
  const std::vector<Json>& items() const { return items_; }

  std::string dump() const;

 private:
  void dump_to(std::string* out) const;

  Type type_ = Type::kNull;
  bool boolean_ = false;
  double number_ = 0.0;
  std::string text_;
  std::vector<Json> items_;
  std::vector<std::pair<std::string, Json>> fields_;
};

// Escapes a UTF-8 string into a JSON string literal, quotes included.
std::string json_quote(const std::string& text);

}  // namespace ghost
