#include "redis_writer.h"

#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>
#include <sw/redis++/redis++.h>

#include "lm/clock.h"

namespace lm::manager {

namespace {

std::string fmt_float(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v));
  return buf;
}

}  // namespace

void RedisWriter::parse_uri(const std::string& uri, sw::redis::ConnectionOptions& co) {
  std::string rest = uri;
  const std::string scheme = "tcp://";
  if (rest.rfind(scheme, 0) == 0) rest = rest.substr(scheme.size());
  if (rest.find("://") != std::string::npos) throw std::invalid_argument("只支持 tcp:// 的 Redis 地址: " + uri);
  const auto colon = rest.rfind(':');
  co.host = rest.substr(0, colon);
  co.port = 6379;
  if (colon != std::string::npos) {
    try {
      co.port = std::stoi(rest.substr(colon + 1));
    } catch (const std::exception&) {
      throw std::invalid_argument("Redis 地址端口无效: " + uri);
    }
  }
  if (co.host.empty()) throw std::invalid_argument("Redis 地址缺少主机: " + uri);
}

RedisWriter::RedisWriter(StorageQueue& queue, Options opt) : queue_(queue), opt_(std::move(opt)) {
  sw::redis::ConnectionOptions co;
  parse_uri(opt_.uri, co);
  if (!opt_.password.empty()) co.password = opt_.password;
  co.connect_timeout = std::chrono::milliseconds(500);
  co.socket_timeout = std::chrono::milliseconds(500);
  sw::redis::ConnectionPoolOptions po;
  po.size = 1;  // 只有存储线程使用
  redis_ = std::make_unique<sw::redis::Redis>(co, po);
}

RedisWriter::~RedisWriter() { stop(); }

void RedisWriter::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void RedisWriter::stop() {
  if (!running_.exchange(false)) return;
  queue_.close();
  if (thread_.joinable()) thread_.join();
}

std::string RedisWriter::to_json(const lm::Sample& s) {
  std::string out;
  google::protobuf::util::JsonPrintOptions o;
  o.preserve_proto_field_names = true;
  (void)google::protobuf::util::MessageToJsonString(s, &out, o);
  return out;
}

void RedisWriter::on_error(const char* what, const std::exception& e) {
  errors_.fetch_add(1);
  uint64_t suppressed = 0;
  if (err_log_.allow(&suppressed)) {
    spdlog::warn("Redis {} 失败: {}（此前 10 秒内另有 {} 次失败未记录）", what, e.what(),
                 suppressed);
  }
}

void RedisWriter::write_batch(const std::vector<AcceptedSample>& batch) {
  if (batch.empty()) return;
  try {
    auto pipe = redis_->pipeline(false);
    for (const auto& a : batch) {
      const lm::Sample& s = a.sample;
      const std::string json = to_json(s);
      const std::string hkey = key("node:" + s.node_id());
      const std::map<std::string, std::string> fields = {
          {"ts_ms", std::to_string(s.ts_ms())},
          {"seq", std::to_string(s.seq())},
          {"addr", a.addr},
          {"ncpu", std::to_string(a.ncpu)},
          {"cpu_busy", fmt_float(s.cpu_busy())},
          {"mem_avail_ratio", fmt_float(s.mem_avail_ratio())},
          {"disk_util", fmt_float(s.disk_util())},
          {"last_seen_ms", std::to_string(a.recv_wall_ms)},
      };
      pipe.hset(hkey, fields.begin(), fields.end());
      pipe.zadd(key("node:" + s.node_id() + ":ts"), json, static_cast<double>(s.ts_ms()));
      pending_stream_[s.node_id()] = json;
      known_nodes_[s.node_id()] = true;
    }
    pipe.exec();
  } catch (const sw::redis::Error& e) {
    on_error("写入", e);
  }
}

void RedisWriter::flush_stream(int64_t now_wall_ms) {
  if (pending_stream_.empty()) return;
  std::string nodes = "[";
  bool first = true;
  for (const auto& [id, json] : pending_stream_) {
    if (!first) nodes += ',';
    nodes += json;
    first = false;
  }
  nodes += ']';
  pending_stream_.clear();  // 失败也清空：不积压过期数据
  try {
    const std::vector<std::pair<std::string, std::string>> fields = {
        {"ts_ms", std::to_string(now_wall_ms)}, {"nodes", nodes}};
    redis_->xadd(key("stream:metrics"), "*", fields.begin(), fields.end(), opt_.stream_maxlen,
                 true);
  } catch (const sw::redis::Error& e) {
    on_error("XADD", e);
  }
}

void RedisWriter::trim(int64_t now_wall_ms) {
  if (known_nodes_.empty()) return;
  const double cutoff = static_cast<double>(now_wall_ms - opt_.ts_retention_ms);
  try {
    auto pipe = redis_->pipeline(false);
    for (const auto& [id, _] : known_nodes_) {
      pipe.zremrangebyscore(key("node:" + id + ":ts"),
                            sw::redis::BoundedInterval<double>(
                                0, cutoff, sw::redis::BoundType::RIGHT_OPEN));
    }
    pipe.exec();
  } catch (const sw::redis::Error& e) {
    on_error("修剪", e);
  }
}

void RedisWriter::run() {
  auto next_stream = std::chrono::steady_clock::now() + opt_.stream_interval;
  auto next_trim = std::chrono::steady_clock::now() + opt_.trim_interval;
  while (running_.load(std::memory_order_relaxed)) {
    write_batch(queue_.pop_batch(opt_.batch_max, opt_.flush_interval));
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_stream) {
      flush_stream(lm::wall_ms());
      next_stream = now + opt_.stream_interval;
    }
    if (now >= next_trim) {
      trim(lm::wall_ms());
      next_trim = now + opt_.trim_interval;
    }
  }
  write_batch(queue_.pop_batch(opt_.batch_max, std::chrono::milliseconds(0)));  // 退出前尽量写完
}

}  // namespace lm::manager
