#pragma once

// 生成 JSON 的小工具：事件 detail、MySQL JSON 列用。只做转义与拼接，不解析。

#include <cmath>
#include <cstdio>
#include <string>

namespace lm::manager {

inline std::string json_str(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

inline std::string json_num(double v) {
  if (!std::isfinite(v)) return "null";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

// 逐项拼接 JSON 对象：JsonObj().add("a", 1).add_str("b", "x").str() → {"a":1,"b":"x"}
class JsonObj {
 public:
  JsonObj& add(const std::string& k, double v) { return raw(k, json_num(v)); }
  JsonObj& add_int(const std::string& k, long long v) { return raw(k, std::to_string(v)); }
  JsonObj& add_str(const std::string& k, const std::string& v) { return raw(k, json_str(v)); }
  JsonObj& raw(const std::string& k, const std::string& json) {
    if (!body_.empty()) body_ += ',';
    body_ += json_str(k) + ':' + json;
    return *this;
  }
  std::string str() const { return "{" + body_ + "}"; }

 private:
  std::string body_;
};

}  // namespace lm::manager
