// minijson.h — dependency-free JSON parser (object / array / string / number / bool / null).
//
// Why not vendor nlohmann/json? It is a ~900KB single header and the raw.githubusercontent
// mirror is unreachable from this machine. The shim only needs to read a flat profile
// document, so a compact recursive-descent parser keeps the DLL small and the build hermetic.
#pragma once

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mj {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
  enum Type { kNull, kBool, kNum, kStr, kArr, kObj };

  Type type = kNull;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::shared_ptr<Array> arr;
  std::shared_ptr<Object> obj;

  const Value* find(const std::string& key) const {
    if (type != kObj || !obj) return nullptr;
    auto it = obj->find(key);
    return it == obj->end() ? nullptr : &it->second;
  }

  std::string as_str(const std::string& fallback = std::string()) const {
    return type == kStr ? str : fallback;
  }
  double as_num(double fallback = 0.0) const {
    if (type == kNum) return num;
    if (type == kBool) return b ? 1.0 : 0.0;
    return fallback;
  }
  bool as_bool(bool fallback = false) const { return type == kBool ? b : fallback; }
};

class Parser {
 public:
  explicit Parser(const std::string& src) : s_(src) {}

  bool parse(Value& out) {
    skip();
    if (!value(out)) return false;
    skip();
    return true;
  }

 private:
  const std::string& s_;
  size_t i_ = 0;

  void skip() {
    while (i_ < s_.size()) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++i_;
        continue;
      }
      break;
    }
  }

  bool lit(const char* word, size_t n) {
    if (s_.compare(i_, n, word) != 0) return false;
    i_ += n;
    return true;
  }

  bool value(Value& v) {
    skip();
    if (i_ >= s_.size()) return false;
    switch (s_[i_]) {
      case '{': return object(v);
      case '[': return array(v);
      case '"':
        v.type = Value::kStr;
        return string(v.str);
      case 't':
        if (lit("true", 4)) { v.type = Value::kBool; v.b = true; return true; }
        return false;
      case 'f':
        if (lit("false", 5)) { v.type = Value::kBool; v.b = false; return true; }
        return false;
      case 'n':
        if (lit("null", 4)) { v.type = Value::kNull; return true; }
        return false;
      default: return number(v);
    }
  }

  bool object(Value& v) {
    v.type = Value::kObj;
    v.obj = std::make_shared<Object>();
    ++i_;  // consume '{'
    skip();
    if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
    while (i_ < s_.size()) {
      skip();
      if (i_ >= s_.size() || s_[i_] != '"') return false;
      std::string key;
      if (!string(key)) return false;
      skip();
      if (i_ >= s_.size() || s_[i_] != ':') return false;
      ++i_;
      Value child;
      if (!value(child)) return false;
      (*v.obj)[key] = std::move(child);
      skip();
      if (i_ >= s_.size()) return false;
      if (s_[i_] == ',') { ++i_; continue; }
      if (s_[i_] == '}') { ++i_; return true; }
      return false;
    }
    return false;
  }

  bool array(Value& v) {
    v.type = Value::kArr;
    v.arr = std::make_shared<Array>();
    ++i_;  // consume '['
    skip();
    if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
    while (i_ < s_.size()) {
      Value child;
      if (!value(child)) return false;
      v.arr->push_back(std::move(child));
      skip();
      if (i_ >= s_.size()) return false;
      if (s_[i_] == ',') { ++i_; continue; }
      if (s_[i_] == ']') { ++i_; return true; }
      return false;
    }
    return false;
  }

  static void utf8(std::string& out, unsigned cp) {
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
  }

  bool hex4(unsigned& out) {
    if (i_ + 4 > s_.size()) return false;
    out = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = s_[i_++];
      out <<= 4;
      if (h >= '0' && h <= '9') out |= static_cast<unsigned>(h - '0');
      else if (h >= 'a' && h <= 'f') out |= static_cast<unsigned>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') out |= static_cast<unsigned>(h - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool string(std::string& out) {
    ++i_;  // consume '"'
    out.clear();
    while (i_ < s_.size()) {
      const unsigned char c = static_cast<unsigned char>(s_[i_++]);
      if (c == '"') return true;
      if (c != '\\') { out += static_cast<char>(c); continue; }
      if (i_ >= s_.size()) return false;
      const char e = s_[i_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' &&
              s_[i_ + 1] == 'u') {
            i_ += 2;
            unsigned lo = 0;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool number(Value& v) {
    const size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
    bool any = false;
    while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; any = true; }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; any = true; }
    }
    if (!any) return false;
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      ++i_;
      if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
      while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
    }
    v.type = Value::kNum;
    v.num = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
    return true;
  }
};

inline bool parse(const std::string& text, Value& out) { return Parser(text).parse(out); }

}  // namespace mj
