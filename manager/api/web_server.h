#pragma once

// 看板接口（M7）：Drogon 在 Manager 进程内另起线程，提供 REST 与 WebSocket /ws。
// 节点数据来自 NodeTable（内存），曲线与事件来自 Redis（node:{id}:ts、stream:events）。
// 编译时没有找到 Drogon（未定义 LM_HAVE_DROGON）时为桩：start 只记一条警告。

#include <cstdint>
#include <memory>
#include <string>

#include "ingest/node_table.h"

namespace lm::manager {

class WebServer {
 public:
  struct Options {
    bool enabled = true;
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    std::string token;        // 为空不鉴权
    std::size_t threads = 2;
    std::string redis_uri = "tcp://127.0.0.1:6379";
    std::string redis_password;
    std::string key_prefix;   // 测试用
    long long backfill_max = 10000;
  };

  WebServer(NodeTable& table, Options opt);
  ~WebServer();
  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  static bool compiled_in();
  void start();
  void stop();
  std::size_t connections() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace lm::manager
