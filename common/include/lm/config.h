#pragma once

// YAML 配置读取：按 "a.b.c" 路径取值，缺失时用缺省值，类型不对时抛出带键名的异常。

#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace lm {

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Config {
 public:
  Config() = default;
  explicit Config(YAML::Node root) : root_(std::move(root)) {}

  // 读取文件；文件不存在或语法错误时抛 ConfigError。
  static Config load_file(const std::string& path);
  // 从字符串解析（测试用）。
  static Config load_string(const std::string& text);

  bool has(const std::string& key) const { return find(key).IsDefined(); }

  template <typename T>
  T get(const std::string& key, const T& fallback) const {
    YAML::Node node = find(key);
    if (!node.IsDefined() || node.IsNull()) return fallback;
    try {
      return node.as<T>();
    } catch (const YAML::Exception&) {
      throw ConfigError("配置项 " + key + " 的类型不正确，值为: " + YAML::Dump(node));
    }
  }

  template <typename T>
  T require(const std::string& key) const {
    YAML::Node node = find(key);
    if (!node.IsDefined() || node.IsNull()) throw ConfigError("缺少配置项 " + key);
    return get<T>(key, T{});
  }

 private:
  YAML::Node find(const std::string& key) const;

  YAML::Node root_;
};

}  // namespace lm
