#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ghost {
namespace {

void append_utf8(std::string* out, unsigned int code_point) {
  if (code_point <= 0x7F) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct Parser {
  const std::string& text;
  size_t pos = 0;
  std::string* error;

  bool fail(const std::string& message) {
    if (error->empty()) *error = message;
    return false;
  }

  void skip_space() {
    while (pos < text.size()) {
      const char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        pos++;
      } else {
        break;
      }
    }
  }

  bool literal(const char* word) {
    const size_t n = std::char_traits<char>::length(word);
    if (text.compare(pos, n, word) != 0) return false;
    pos += n;
    return true;
  }

  bool parse_string(std::string* out) {
    if (pos >= text.size() || text[pos] != '"') return fail("expected a string");
    pos++;
    out->clear();
    while (pos < text.size()) {
      const char c = text[pos++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos >= text.size()) return fail("truncated escape");
      const char e = text[pos++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          if (pos + 4 > text.size()) return fail("truncated \\u escape");
          unsigned int code = 0;
          for (int k = 0; k < 4; k++) {
            const int d = hex_digit(text[pos + k]);
            if (d < 0) return fail("bad \\u escape");
            code = code * 16 + static_cast<unsigned int>(d);
          }
          pos += 4;
          // Surrogate pairs arrive as two escapes and must be recombined.
          if (code >= 0xD800 && code <= 0xDBFF && pos + 6 <= text.size() &&
              text[pos] == '\\' && text[pos + 1] == 'u') {
            unsigned int low = 0;
            bool ok = true;
            for (int k = 0; k < 4; k++) {
              const int d = hex_digit(text[pos + 2 + k]);
              if (d < 0) { ok = false; break; }
              low = low * 16 + static_cast<unsigned int>(d);
            }
            if (ok && low >= 0xDC00 && low <= 0xDFFF) {
              pos += 6;
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          append_utf8(out, code);
          break;
        }
        default:
          return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool parse_value(Json* out) {
    skip_space();
    if (pos >= text.size()) return fail("unexpected end of input");
    const char c = text[pos];
    if (c == '{') {
      pos++;
      *out = Json::object();
      skip_space();
      if (pos < text.size() && text[pos] == '}') { pos++; return true; }
      while (true) {
        skip_space();
        std::string key;
        if (!parse_string(&key)) return false;
        skip_space();
        if (pos >= text.size() || text[pos] != ':') return fail("expected ':'");
        pos++;
        Json value;
        if (!parse_value(&value)) return false;
        out->set(key, value);
        skip_space();
        if (pos < text.size() && text[pos] == ',') { pos++; continue; }
        if (pos < text.size() && text[pos] == '}') { pos++; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      pos++;
      *out = Json::array();
      skip_space();
      if (pos < text.size() && text[pos] == ']') { pos++; return true; }
      while (true) {
        Json value;
        if (!parse_value(&value)) return false;
        out->push(value);
        skip_space();
        if (pos < text.size() && text[pos] == ',') { pos++; continue; }
        if (pos < text.size() && text[pos] == ']') { pos++; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      std::string s;
      if (!parse_string(&s)) return false;
      *out = Json::string(s);
      return true;
    }
    if (literal("true")) { *out = Json::boolean(true); return true; }
    if (literal("false")) { *out = Json::boolean(false); return true; }
    if (literal("null")) { *out = Json(); return true; }

    const char* start = text.c_str() + pos;
    char* end = nullptr;
    const double value = std::strtod(start, &end);
    if (end == start) return fail("expected a value");
    pos += static_cast<size_t>(end - start);
    *out = Json::number(value);
    return true;
  }
};

}  // namespace

Json Json::boolean(bool value) {
  Json j;
  j.type_ = Type::kBool;
  j.boolean_ = value;
  return j;
}

Json Json::number(double value) {
  Json j;
  j.type_ = Type::kNumber;
  j.number_ = value;
  return j;
}

Json Json::integer(long long value) {
  Json j;
  j.type_ = Type::kNumber;
  j.number_ = static_cast<double>(value);
  return j;
}

Json Json::string(const std::string& value) {
  Json j;
  j.type_ = Type::kString;
  j.text_ = value;
  return j;
}

Json Json::array() {
  Json j;
  j.type_ = Type::kArray;
  return j;
}

Json Json::object() {
  Json j;
  j.type_ = Type::kObject;
  return j;
}

Json Json::parse(const std::string& text, std::string* error) {
  error->clear();
  Parser parser{text, 0, error};
  Json value;
  if (!parser.parse_value(&value)) return Json();
  parser.skip_space();
  if (parser.pos != text.size()) {
    *error = "trailing data after the JSON value";
    return Json();
  }
  return value;
}

bool Json::as_bool(bool fallback) const {
  if (type_ == Type::kBool) return boolean_;
  if (type_ == Type::kNumber) return number_ != 0.0;
  return fallback;
}

double Json::as_number(double fallback) const {
  if (type_ == Type::kNumber) return number_;
  if (type_ == Type::kString) {
    char* end = nullptr;
    const double v = std::strtod(text_.c_str(), &end);
    if (end != text_.c_str()) return v;
  }
  return fallback;
}

std::string Json::as_string(const std::string& fallback) const {
  if (type_ == Type::kString) return text_;
  if (type_ == Type::kBool) return boolean_ ? "true" : "false";
  if (type_ == Type::kNumber) {
    char buf[32];
    if (number_ == std::floor(number_) && std::fabs(number_) < 1e15) {
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number_));
    } else {
      std::snprintf(buf, sizeof(buf), "%.17g", number_);
    }
    return std::string(buf);
  }
  return fallback;
}

const Json* Json::find(const std::string& key) const {
  for (const auto& field : fields_) {
    if (field.first == key) return &field.second;
  }
  return nullptr;
}

void Json::set(const std::string& key, Json value) {
  for (auto& field : fields_) {
    if (field.first == key) {
      field.second = std::move(value);
      return;
    }
  }
  fields_.emplace_back(key, std::move(value));
}

void Json::push(Json value) { items_.push_back(std::move(value)); }

std::string json_quote(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

void Json::dump_to(std::string* out) const {
  switch (type_) {
    case Type::kNull:
      *out += "null";
      break;
    case Type::kBool:
      *out += boolean_ ? "true" : "false";
      break;
    case Type::kNumber:
      // Integral values are printed without a decimal point so that ids, pids
      // and pixel coordinates survive a round trip through a client that does
      // not distinguish 1 from 1.0.
      if (number_ == std::floor(number_) && std::fabs(number_) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(number_));
        *out += buf;
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", number_);
        *out += buf;
      }
      break;
    case Type::kString:
      *out += json_quote(text_);
      break;
    case Type::kArray: {
      *out += "[";
      for (size_t i = 0; i < items_.size(); i++) {
        if (i != 0) *out += ",";
        items_[i].dump_to(out);
      }
      *out += "]";
      break;
    }
    case Type::kObject: {
      *out += "{";
      for (size_t i = 0; i < fields_.size(); i++) {
        if (i != 0) *out += ",";
        *out += json_quote(fields_[i].first);
        *out += ":";
        fields_[i].second.dump_to(out);
      }
      *out += "}";
      break;
    }
  }
}

std::string Json::dump() const {
  std::string out;
  dump_to(&out);
  return out;
}

}  // namespace ghost
