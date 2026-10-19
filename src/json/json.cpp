#include "npk/json/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace npk::json {

std::optional<bool> Value::as_bool() const {
  if (const bool* b = std::get_if<bool>(&v_)) return *b;
  return std::nullopt;
}
std::optional<std::int64_t> Value::as_int() const {
  if (const auto* i = std::get_if<std::int64_t>(&v_)) return *i;
  if (const auto* d = std::get_if<double>(&v_)) {
    if (std::isfinite(*d) && std::floor(*d) == *d && std::fabs(*d) < 9.0e15) return static_cast<std::int64_t>(*d);
  }
  return std::nullopt;
}
std::optional<double> Value::as_double() const {
  if (const auto* d = std::get_if<double>(&v_)) return *d;
  if (const auto* i = std::get_if<std::int64_t>(&v_)) return static_cast<double>(*i);
  return std::nullopt;
}
const std::string* Value::as_string() const { return std::get_if<std::string>(&v_); }
const Value::Array* Value::as_array() const { return std::get_if<Array>(&v_); }
const Value::Object* Value::as_object() const { return std::get_if<Object>(&v_); }

const Value* Value::find(std::string_view key) const {
  if (const auto* o = std::get_if<Object>(&v_)) {
    for (const auto& kv : *o) {
      if (kv.first == key) return &kv.second;
    }
  }
  return nullptr;
}

Value& Value::push_back(Value v) {
  if (!is_array()) v_ = Array{};
  auto& a = std::get<Array>(v_);
  a.push_back(std::move(v));
  return a.back();
}

Value& Value::set(std::string key, Value v) {
  if (!is_object()) v_ = Object{};
  auto& o = std::get<Object>(v_);
  for (auto& kv : o) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return kv.second;
    }
  }
  o.emplace_back(std::move(key), std::move(v));
  return o.back().second;
}

namespace {

class Parser {
 public:
  Parser(std::string_view t, const Limits& l) : t_(t), lim_(l) {}

  std::optional<Value> run(std::string* error) {
    Value v;
    if (t_.size() > lim_.max_bytes) return fail("document too large", error);
    skip_ws();
    if (!value(v, 1)) return fail(err_, error);
    skip_ws();
    if (i_ != t_.size()) return fail("trailing characters", error);
    return v;
  }

 private:
  std::optional<Value> fail(std::string_view why, std::string* error) {
    if (error) *error = std::string(why) + " at byte " + std::to_string(i_);
    return std::nullopt;
  }
  bool bad(const char* why) {
    err_ = why;
    return false;
  }
  void skip_ws() {
    while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
  }
  bool eat(char c) {
    if (i_ < t_.size() && t_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  bool literal(std::string_view word) {
    if (t_.substr(i_, word.size()) != word) return false;
    i_ += word.size();
    return true;
  }

  bool value(Value& out, std::size_t depth) {
    if (depth > lim_.max_depth) return bad("nested too deeply");
    if (++count_ > lim_.max_values) return bad("too many values");
    if (i_ >= t_.size()) return bad("unexpected end");
    switch (t_[i_]) {
      case '{': return object(out, depth);
      case '[': return array(out, depth);
      case '"': {
        std::string s;
        if (!string(s)) return false;
        out = Value(std::move(s));
        return true;
      }
      case 't': if (!literal("true")) return bad("bad literal"); out = Value(true); return true;
      case 'f': if (!literal("false")) return bad("bad literal"); out = Value(false); return true;
      case 'n': if (!literal("null")) return bad("bad literal"); out = Value(); return true;
      default: return number(out);
    }
  }

  bool object(Value& out, std::size_t depth) {
    ++i_;  // {
    Value::Object members;
    skip_ws();
    if (eat('}')) {
      out = Value(std::move(members));
      return true;
    }
    for (;;) {
      skip_ws();
      if (i_ >= t_.size() || t_[i_] != '"') return bad("expected a key");
      std::string key;
      if (!string(key)) return false;
      for (const auto& kv : members) {
        if (kv.first == key) return bad("duplicate key");
      }
      skip_ws();
      if (!eat(':')) return bad("expected ':'");
      skip_ws();
      Value v;
      if (!value(v, depth + 1)) return false;
      members.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (eat(',')) continue;
      if (eat('}')) break;
      return bad("expected ',' or '}'");
    }
    out = Value(std::move(members));
    return true;
  }

  bool array(Value& out, std::size_t depth) {
    ++i_;  // [
    Value::Array items;
    skip_ws();
    if (eat(']')) {
      out = Value(std::move(items));
      return true;
    }
    for (;;) {
      skip_ws();
      Value v;
      if (!value(v, depth + 1)) return false;
      items.push_back(std::move(v));
      skip_ws();
      if (eat(',')) continue;
      if (eat(']')) break;
      return bad("expected ',' or ']'");
    }
    out = Value(std::move(items));
    return true;
  }

  static void append_utf8(std::string& s, std::uint32_t cp) {
    if (cp < 0x80) {
      s += static_cast<char>(cp);
    } else if (cp < 0x800) {
      s += static_cast<char>(0xc0 | (cp >> 6));
      s += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      s += static_cast<char>(0xe0 | (cp >> 12));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      s += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      s += static_cast<char>(0xf0 | (cp >> 18));
      s += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      s += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }

  bool hex4(std::uint32_t& v) {
    if (t_.size() - i_ < 4) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = t_[i_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
      else return false;
    }
    return true;
  }

  // Validates one UTF-8 sequence starting at i_ (lead byte >= 0x80) and copies it.
  bool utf8(std::string& s) {
    const auto b0 = static_cast<std::uint8_t>(t_[i_]);
    std::size_t n;
    std::uint32_t cp;
    if (b0 >= 0xc2 && b0 <= 0xdf) { n = 2; cp = b0 & 0x1f; }
    else if (b0 >= 0xe0 && b0 <= 0xef) { n = 3; cp = b0 & 0x0f; }
    else if (b0 >= 0xf0 && b0 <= 0xf4) { n = 4; cp = b0 & 0x07; }
    else return bad("invalid UTF-8");
    if (t_.size() - i_ < n) return bad("invalid UTF-8");
    for (std::size_t k = 1; k < n; ++k) {
      const auto b = static_cast<std::uint8_t>(t_[i_ + k]);
      if ((b & 0xc0) != 0x80) return bad("invalid UTF-8");
      cp = (cp << 6) | (b & 0x3f);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return bad("invalid UTF-8");
    s.append(t_.substr(i_, n));
    i_ += n;
    return true;
  }

  bool string(std::string& out) {
    ++i_;  // "
    for (;;) {
      if (i_ >= t_.size()) return bad("unterminated string");
      if (out.size() > lim_.max_string) return bad("string too long");
      const auto c = static_cast<std::uint8_t>(t_[i_]);
      if (c == '"') {
        ++i_;
        return out.size() <= lim_.max_string || bad("string too long");
      }
      if (c < 0x20) return bad("control character in string");
      if (c >= 0x80) {
        if (!utf8(out)) return false;
        continue;
      }
      if (c != '\\') {
        out += static_cast<char>(c);
        ++i_;
        continue;
      }
      if (++i_ >= t_.size()) return bad("unterminated string");
      const char e = t_[i_++];
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
          std::uint32_t cp;
          if (!hex4(cp)) return bad("bad \\u escape");
          if (cp >= 0xd800 && cp <= 0xdbff) {
            std::uint32_t lo;
            if (!(eat('\\') && eat('u') && hex4(lo)) || lo < 0xdc00 || lo > 0xdfff) return bad("lone surrogate");
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return bad("lone surrogate");
          }
          if (cp == 0) return bad("NUL in string");
          append_utf8(out, cp);
          break;
        }
        default: return bad("bad escape");
      }
    }
  }

  bool number(Value& out) {
    const std::size_t start = i_;
    eat('-');
    if (i_ >= t_.size()) return bad("bad number");
    if (t_[i_] == '0') {
      ++i_;
    } else if (t_[i_] >= '1' && t_[i_] <= '9') {
      while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
    } else {
      return bad("unexpected character");
    }
    bool integral = true;
    if (eat('.')) {
      integral = false;
      const std::size_t d = i_;
      while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
      if (i_ == d) return bad("bad number");
    }
    if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
      integral = false;
      ++i_;
      if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
      const std::size_t d = i_;
      while (i_ < t_.size() && t_[i_] >= '0' && t_[i_] <= '9') ++i_;
      if (i_ == d) return bad("bad number");
    }
    if (i_ - start > 40) return bad("number too long");
    const char* b = t_.data() + start;
    const char* e = t_.data() + i_;
    if (integral) {
      std::int64_t v;
      auto r = std::from_chars(b, e, v);
      if (r.ec == std::errc() && r.ptr == e) {
        out = Value(v);
        return true;
      }
    }
    double d;
    auto r = std::from_chars(b, e, d);
    if (r.ec != std::errc() || r.ptr != e || !std::isfinite(d)) return bad("number out of range");
    out = Value(d);
    return true;
  }

  std::string_view t_;
  Limits lim_;
  std::size_t i_ = 0, count_ = 0;
  const char* err_ = "";
};

void write_string(std::string& o, const std::string& s) {
  o += '"';
  for (const char ch : s) {
    const auto c = static_cast<std::uint8_t>(ch);
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          o += buf;
        } else {
          o += ch;
        }
    }
  }
  o += '"';
}

void write(std::string& o, const Value& v) {
  if (v.is_null()) o += "null";
  else if (auto b = v.as_bool(); v.is_bool()) o += *b ? "true" : "false";
  else if (v.is_int()) o += std::to_string(*v.as_int());
  else if (v.is_number()) {
    const double d = *v.as_double();
    if (!std::isfinite(d)) {
      o += "null";
    } else {
      char buf[40];
      std::snprintf(buf, sizeof buf, "%.17g", d);
      o += buf;
    }
  } else if (const auto* s = v.as_string()) write_string(o, *s);
  else if (const auto* a = v.as_array()) {
    o += '[';
    for (std::size_t i = 0; i < a->size(); ++i) {
      if (i) o += ',';
      write(o, (*a)[i]);
    }
    o += ']';
  } else if (const auto* ob = v.as_object()) {
    o += '{';
    bool first = true;
    for (const auto& kv : *ob) {
      if (!first) o += ',';
      first = false;
      write_string(o, kv.first);
      o += ':';
      write(o, kv.second);
    }
    o += '}';
  }
}

}  // namespace

std::optional<Value> parse(std::string_view text, const Limits& limits, std::string* error) {
  return Parser(text, limits).run(error);
}

std::string dump(const Value& v) {
  std::string o;
  write(o, v);
  return o;
}

}  // namespace npk::json
