#include "Core/Json.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace json {

namespace {

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

class Parser {
public:
  explicit Parser(const std::string& text) : s_(text) {}

  Value run() {
    skip_ws();
    Value v = parse_value();
    skip_ws();
    if (pos_ != s_.size()) fail("JSON 末尾有多余内容");
    return v;
  }

private:
  const std::string& s_;
  size_t pos_ = 0;

  [[noreturn]] void fail(const std::string& msg) const {
    throw std::runtime_error("JSON 解析失败 @ " + std::to_string(pos_) + ": " + msg);
  }

  void skip_ws() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
      ++pos_;
  }

  char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

  Value parse_value() {
    char c = peek();
    switch (c) {
      case '{':
        return parse_object();
      case '[':
        return parse_array();
      case '"':
        return parse_string();
      case 't':
        expect("true");
        return Value::make_bool(true);
      case 'f':
        expect("false");
        return Value::make_bool(false);
      case 'n':
        expect("null");
        return Value::make_null();
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        fail(std::string("意外的字符 '") + c + "'");
    }
  }

  void expect(const char* word) {
    for (const char* p = word; *p; ++p) {
      if (pos_ >= s_.size() || s_[pos_] != *p) fail(std::string("期望 ") + word);
      ++pos_;
    }
  }

  Value parse_object() {
    ++pos_;  // '{'
    Value obj = Value::make_object();
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return obj;
    }
    while (true) {
      skip_ws();
      if (peek() != '"') fail("对象 key 必须是字符串");
      Value key = parse_string();
      skip_ws();
      if (peek() != ':') fail("对象缺少 ':'");
      ++pos_;
      skip_ws();
      Value val = parse_value();
      obj.set(key.string, std::move(val));
      skip_ws();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        return obj;
      }
      fail("对象成员间缺少 ',' 或 '}'");
    }
  }

  Value parse_array() {
    ++pos_;  // '['
    Value arr = Value::make_array();
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return arr;
    }
    while (true) {
      skip_ws();
      arr.push_back(parse_value());
      skip_ws();
      char c = peek();
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        return arr;
      }
      fail("数组成员间缺少 ',' 或 ']'");
    }
  }

  Value parse_string() {
    ++pos_;  // '"'
    std::string out;
    while (true) {
      if (pos_ >= s_.size()) fail("字符串未闭合");
      unsigned char c = static_cast<unsigned char>(s_[pos_]);
      if (c == '"') {
        ++pos_;
        return Value::make_string(std::move(out));
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= s_.size()) fail("转义序列未闭合");
        char e = s_[pos_++];
        switch (e) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
            out += parse_unicode_escape();
            break;
          default:
            fail("未知转义 '\\" + std::string(1, e) + "'");
        }
        continue;
      }
      if (c < 0x20) fail("字符串含未转义控制字符");
      // UTF-8 原始字节透传
      out.push_back(static_cast<char>(c));
      ++pos_;
    }
  }

  // 解析 \uXXXX,并把码点(含代理对)编码为 UTF-8。
  std::string parse_unicode_escape() {
    uint32_t cp = read_hex4();
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      // 高代理,期待 \uDC00-\uDFFF
      if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
        pos_ += 2;
        uint32_t lo = read_hex4();
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else {
          return encode_utf8(0xFFFD);
        }
      } else {
        return encode_utf8(0xFFFD);
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      return encode_utf8(0xFFFD);
    }
    return encode_utf8(cp);
  }

  uint32_t read_hex4() {
    if (pos_ + 4 > s_.size()) fail("\\u 需要 4 位十六进制");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<uint32_t>(c - 'A' + 10);
      else
        fail("\\u 含非十六进制字符");
    }
    return v;
  }

  static std::string encode_utf8(uint32_t cp) {
    std::string out;
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
    return out;
  }

  Value parse_number() {
    size_t start = pos_;
    if (peek() == '-') ++pos_;
    if (peek() == '0') {
      ++pos_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (peek() >= '0' && peek() <= '9') ++pos_;
    } else {
      fail("非法数字");
    }
    if (peek() == '.') {
      ++pos_;
      if (!(peek() >= '0' && peek() <= '9')) fail("小数部分缺少数字");
      while (peek() >= '0' && peek() <= '9') ++pos_;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') ++pos_;
      if (!(peek() >= '0' && peek() <= '9')) fail("指数部分缺少数字");
      while (peek() >= '0' && peek() <= '9') ++pos_;
    }
    std::string num = s_.substr(start, pos_ - start);
    return Value::make_number(std::strtod(num.c_str(), nullptr));
  }
};

// ---------------------------------------------------------------------------
// 写出
// ---------------------------------------------------------------------------

void append_escaped(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));  // UTF-8 原样
        }
    }
  }
  out.push_back('"');
}

void append_number(std::string& out, double d) {
  if (d == static_cast<long long>(d) && d >= -9.2e18 && d <= 9.2e18) {
    out += std::to_string(static_cast<long long>(d));
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", d);
    out += buf;
  }
}

void write_impl(const Value& v, std::string& out, int depth, bool indent) {
  switch (v.type) {
    case Value::Null:
      out += "null";
      break;
    case Value::Bool:
      out += v.boolean ? "true" : "false";
      break;
    case Value::Number:
      append_number(out, v.number);
      break;
    case Value::String:
      append_escaped(out, v.string);
      break;
    case Value::Array: {
      if (v.array.empty()) {
        out += "[]";
        break;
      }
      out.push_back('[');
      for (size_t i = 0; i < v.array.size(); ++i) {
        if (i) out.push_back(',');
        if (indent) {
          out.push_back('\n');
          out.append(static_cast<size_t>((depth + 1) * 2), ' ');
        }
        write_impl(v.array[i], out, depth + 1, indent);
      }
      if (indent) {
        out.push_back('\n');
        out.append(static_cast<size_t>(depth * 2), ' ');
      }
      out.push_back(']');
      break;
    }
    case Value::Object: {
      if (v.object.empty()) {
        out += "{}";
        break;
      }
      out.push_back('{');
      for (size_t i = 0; i < v.object.size(); ++i) {
        if (i) out.push_back(',');
        if (indent) {
          out.push_back('\n');
          out.append(static_cast<size_t>((depth + 1) * 2), ' ');
        }
        append_escaped(out, v.object[i].first);
        out += indent ? ": " : ":";
        write_impl(v.object[i].second, out, depth + 1, indent);
      }
      if (indent) {
        out.push_back('\n');
        out.append(static_cast<size_t>(depth * 2), ' ');
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace

Value parse(const std::string& text) {
  return Parser(text).run();
}

std::string write(const Value& v, bool indent) {
  std::string out;
  write_impl(v, out, 0, indent);
  return out;
}

}  // namespace json
