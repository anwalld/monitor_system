#pragma once

// 存储线程：把被接受的上报写入 Redis 热数据。
//   node:{id}       Hash        最新指标与元信息
//   node:{id}:ts    Sorted Set  每秒样本（压缩 JSON），score = ts_ms，保留 1 小时
//   stream:metrics  Stream      每秒一条，含这一秒内所有节点的最新样本
// 写失败只记日志（限频），不重试、不积压。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "ingest/node_table.h"
#include "lm/log.h"
#include "storage_queue.h"

namespace sw::redis {
class Redis;
struct ConnectionOptions;
}

namespace lm::manager {

using StorageQueue = BoundedQueue<AcceptedSample>;

class RedisWriter {
 public:
  struct Options {
    std::string uri = "tcp://127.0.0.1:6379";
    std::string password;              // 为空则不认证
    int64_t ts_retention_ms = 3600 * 1000;
    std::size_t batch_max = 256;
    std::chrono::milliseconds flush_interval{100};
    std::chrono::milliseconds stream_interval{1000};
    std::chrono::milliseconds trim_interval{60 * 1000};
    long long stream_maxlen = 100000;
    std::string key_prefix;            // 测试用，正式环境为空
  };

  RedisWriter(StorageQueue& queue, Options opt);
  ~RedisWriter();

  void start();
  void stop();

  // 以下供测试直接驱动，正常由线程循环调用。
  void write_batch(const std::vector<AcceptedSample>& batch);
  void flush_stream(int64_t now_wall_ms);
  void trim(int64_t now_wall_ms);

  uint64_t write_errors() const { return errors_.load(); }

  // 样本序列化为压缩 JSON（proto 字段名）。
  static std::string to_json(const lm::Sample& s);

  // 解析 "tcp://host:port" 或 "host:port"（端口缺省 6379），填入连接参数；格式错误抛 std::invalid_argument。
  static void parse_uri(const std::string& uri, sw::redis::ConnectionOptions& co);

 private:
  void run();
  void on_error(const char* what, const std::exception& e);
  std::string key(const std::string& k) const { return opt_.key_prefix + k; }

  StorageQueue& queue_;
  const Options opt_;
  std::unique_ptr<sw::redis::Redis> redis_;

  std::map<std::string, std::string> pending_stream_;  // node_id → 这一秒内最新样本 JSON
  std::map<std::string, bool> known_nodes_;            // 需要修剪的节点
  lm::RateLimiter err_log_{std::chrono::seconds(10)};
  std::atomic<uint64_t> errors_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace lm::manager
