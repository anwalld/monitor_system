#include "lm/config.h"

#include <fstream>

namespace lm {

Config Config::load_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw ConfigError("无法打开配置文件 " + path);
  try {
    return Config(YAML::LoadFile(path));
  } catch (const YAML::Exception& e) {
    throw ConfigError("配置文件 " + path + " 解析失败: " + e.what());
  }
}

Config Config::load_string(const std::string& text) {
  try {
    return Config(YAML::Load(text));
  } catch (const YAML::Exception& e) {
    throw ConfigError(std::string("配置解析失败: ") + e.what());
  }
}

YAML::Node Config::find(const std::string& key) const {
  // yaml-cpp 的 operator[] 返回引用语义的节点，逐级 reset 而不是赋值，避免改写原树
  YAML::Node node;
  node.reset(root_);
  std::size_t start = 0;
  while (start <= key.size()) {
    const std::size_t dot = key.find('.', start);
    const std::string part = key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (!node.IsMap()) return YAML::Node(YAML::NodeType::Undefined);
    YAML::Node child = node[part];
    if (!child.IsDefined()) return YAML::Node(YAML::NodeType::Undefined);
    node.reset(child);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return node;
}

}  // namespace lm
