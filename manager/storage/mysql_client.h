#pragma once

// MySQL C API 的薄封装：连接（带超时）、断线后限频重连、执行语句、转义。
// 编译时没有找到 MySQL 客户端库（未定义 LM_HAVE_MYSQL）时为桩：connect 总是失败。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lm::manager {

struct MysqlOptions {
  bool enabled = true;
  std::string host = "127.0.0.1";
  unsigned port = 3306;
  std::string user = "lm";
  std::string password;
  std::string database = "lm";
  unsigned timeout_s = 2;
  int64_t reconnect_interval_ms = 5000;
};

class MysqlClient {
 public:
  explicit MysqlClient(MysqlOptions opt);
  ~MysqlClient();
  MysqlClient(const MysqlClient&) = delete;
  MysqlClient& operator=(const MysqlClient&) = delete;

  static bool compiled_in();

  // 确保已连接；断开时按 reconnect_interval_ms 限频重连。now_ms 为单调时钟。
  bool ensure(int64_t now_ms);
  // 执行一条语句；失败时返回 false，error() 给出原因，连接错误会把连接标记为断开。
  bool exec(const std::string& sql);
  // 执行查询并取回全部行（NULL 为空串）；失败返回空。测试与运维用。
  std::optional<std::vector<std::vector<std::string>>> query(const std::string& sql);
  // 转义字符串内容（不含引号）。未连接时按保守规则转义。
  std::string escape(const std::string& s) const;
  // 带引号的字符串字面量。
  std::string quote(const std::string& s) const { return "'" + escape(s) + "'"; }
  const std::string& error() const { return error_; }
  bool connected() const { return conn_ != nullptr; }

 private:
  void close();

  const MysqlOptions opt_;
  void* conn_ = nullptr;  // MYSQL*
  int64_t last_attempt_ms_ = -1;
  std::string error_;
};

}  // namespace lm::manager
