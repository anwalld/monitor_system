#include "web_server.h"

#include <spdlog/spdlog.h>

#ifdef LM_HAVE_DROGON

#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <google/protobuf/util/json_util.h>
#include <sw/redis++/redis++.h>

#include "lm/clock.h"
#include "lm/log.h"
#include "storage/json_util.h"
#include "storage/redis_writer.h"
#include "web_json.h"

namespace lm::manager {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using drogon::WebSocketConnectionPtr;
using Callback = std::function<void(const HttpResponsePtr&)>;
using Item = std::pair<std::string, StreamFields>;

struct WebServer::Impl {
  NodeTable& table;
  const Options opt;
  std::unique_ptr<sw::redis::Redis> redis;  // REST 与 WebSocket 补发（连接池）
  mutable std::mutex mu;
  std::set<WebSocketConnectionPtr> conns;
  std::atomic<bool> running{false};
  std::thread app_thread, pump_thread, push_thread;
  lm::RateLimiter err_log{std::chrono::seconds(10)};

  Impl(NodeTable& t, Options o) : table(t), opt(std::move(o)) {}

  std::string key(const std::string& k) const { return opt.key_prefix + k; }

  sw::redis::ConnectionOptions conn_opts(std::chrono::milliseconds timeout) const {
    sw::redis::ConnectionOptions co;
    RedisWriter::parse_uri(opt.redis_uri, co);
    if (!opt.redis_password.empty()) co.password = opt.redis_password;
    co.connect_timeout = std::chrono::milliseconds(500);
    co.socket_timeout = timeout;
    return co;
  }

  void redis_error(const char* what, const std::exception& e) {
    uint64_t suppressed = 0;
    if (err_log.allow(&suppressed)) {
      spdlog::warn("看板接口 Redis {} 失败: {}（此前 10 秒内另有 {} 次未记录）", what, e.what(), suppressed);
    }
  }

  // ---------- 鉴权与响应 ----------
  bool authorized(const HttpRequestPtr& req, bool allow_query) const {
    if (opt.token.empty()) return true;
    std::string got = bearer_token(req->getHeader("authorization"));
    if (got.empty() && allow_query) got = req->getParameter("token");
    return token_ok(opt.token, got);
  }
  static HttpResponsePtr json(std::string body, drogon::HttpStatusCode code = drogon::k200OK) {
    auto r = HttpResponse::newHttpResponse();
    r->setStatusCode(code);
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    r->addHeader("Cache-Control", "no-store");
    r->setBody(std::move(body));
    return r;
  }
  static HttpResponsePtr error(drogon::HttpStatusCode code, const std::string& msg) {
    return json(JsonObj().add_str("error", msg).str(), code);
  }
  static long long int_param(const HttpRequestPtr& req, const char* k, long long def, long long lo,
                             long long hi) {
    const std::string v = req->getParameter(k);
    if (v.empty()) return def;
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str()) return def;
    return std::max(lo, std::min(hi, x));
  }

  // ---------- 数据 ----------
  std::string nodes_msg(const char* type, const std::string& last_event_id, bool with_last) const {
    const auto vs = table.views(lm::mono_ms());
    JsonObj o;
    if (type != nullptr) o.add_str("type", type);
    o.add_int("ts", lm::wall_ms());
    if (with_last) o.add_str("last_event_id", last_event_id);
    return o.raw("nodes", nodes_array_json(vs)).str();
  }

  std::string latest_event_id() {
    std::vector<Item> items;
    try {
      redis->xrevrange(key("stream:events"), "+", "-", 1, std::back_inserter(items));
    } catch (const sw::redis::Error& e) {
      redis_error("XREVRANGE", e);
    }
    return items.empty() ? "" : items[0].first;
  }

  void broadcast(const std::string& msg) {
    std::vector<WebSocketConnectionPtr> cs;
    {
      std::lock_guard<std::mutex> lk(mu);
      cs.assign(conns.begin(), conns.end());
    }
    for (auto& c : cs) {
      if (c->connected()) c->send(msg);
    }
  }

  // ---------- WebSocket ----------
  void on_open(const HttpRequestPtr& req, const WebSocketConnectionPtr& conn) {
    if (!authorized(req, true)) {
      conn->shutdown(drogon::CloseCode::kViolation, "unauthorized");
      return;
    }
    // 先加入广播集合，再读补发：补发与实时推送之间没有空档（重叠部分由前端按 id 去重）
    {
      std::lock_guard<std::mutex> lk(mu);
      conns.insert(conn);
    }
    const std::string since = req->getParameter("since");
    if (!since.empty()) {
      std::vector<Item> items;
      try {
        redis->xrange(key("stream:events"), "(" + since, "+", opt.backfill_max, std::back_inserter(items));
      } catch (const sw::redis::Error& e) {
        redis_error("XRANGE 补发", e);
      }
      for (const auto& [id, f] : items) conn->send(event_json(id, f, true));
      spdlog::debug("看板重连，since {}，补发 {} 条事件", since, items.size());
    }
    conn->send(nodes_msg("snapshot", latest_event_id(), true));
  }

  void on_close(const WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lk(mu);
    conns.erase(conn);
  }

  // ---------- 后台线程 ----------
  void pump() {  // 阻塞读 stream:events，推给所有连接
    std::unique_ptr<sw::redis::Redis> r;
    std::string last;
    while (running.load()) {
      try {
        if (!r) r = std::make_unique<sw::redis::Redis>(conn_opts(std::chrono::milliseconds(3000)));
        if (last.empty()) {
          std::vector<Item> items;
          r->xrevrange(key("stream:events"), "+", "-", 1, std::back_inserter(items));
          last = items.empty() ? "0-0" : items[0].first;
        }
        std::unordered_map<std::string, std::vector<Item>> res;
        r->xread(key("stream:events"), last, std::chrono::milliseconds(1000), 256,
                 std::inserter(res, res.end()));
        for (const auto& [stream, items] : res) {
          for (const auto& [id, f] : items) {
            broadcast(event_json(id, f, true));
            last = id;
          }
        }
      } catch (const sw::redis::Error& e) {
        redis_error("XREAD stream:events", e);
        r.reset();
        for (int i = 0; i < 10 && running.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  void push() {  // 每秒一条 node_update（有连接时）
    auto next = std::chrono::steady_clock::now();
    while (running.load()) {
      next += std::chrono::seconds(1);
      std::this_thread::sleep_until(next);
      bool any;
      {
        std::lock_guard<std::mutex> lk(mu);
        any = !conns.empty();
      }
      if (any) broadcast(nodes_msg("node_update", "", false));
    }
  }

  // ---------- REST ----------
  void routes() {
    using drogon::Get;
    auto& app = drogon::app();
    app.registerHandler("/api/health", [this](const HttpRequestPtr&, Callback&& cb) {
      bool ok = true;
      try {
        redis->ping();
      } catch (const sw::redis::Error&) {
        ok = false;
      }
      cb(json(JsonObj().raw("ok", "true").raw("redis", ok ? "true" : "false").str()));
    }, {Get});

    app.registerHandler("/api/nodes", [this](const HttpRequestPtr& req, Callback&& cb) {
      if (!authorized(req, false)) return cb(error(drogon::k401Unauthorized, "unauthorized"));
      cb(json(nodes_msg(nullptr, "", false)));
    }, {Get});

    app.registerHandler("/api/nodes/{id}", [this](const HttpRequestPtr& req, Callback&& cb,
                                                  const std::string& id) {
      if (!authorized(req, false)) return cb(error(drogon::k401Unauthorized, "unauthorized"));
      for (const auto& v : table.views(lm::mono_ms())) {
        if (v.st.node_id == id) return cb(json(node_json(v)));
      }
      cb(error(drogon::k404NotFound, "未知节点 " + id));
    }, {Get});

    app.registerHandler("/api/nodes/{id}/series", [this](const HttpRequestPtr& req, Callback&& cb,
                                                         const std::string& id) {
      if (!authorized(req, false)) return cb(error(drogon::k401Unauthorized, "unauthorized"));
      const long long minutes = int_param(req, "minutes", 60, 1, 60);
      const long long step_s = int_param(req, "step_s", 5, 1, 60);
      const int64_t now = lm::wall_ms();
      std::vector<std::string> members;
      try {
        redis->zrangebyscore(key("node:" + id + ":ts"),
                             sw::redis::BoundedInterval<double>(static_cast<double>(now - minutes * 60000),
                                                                static_cast<double>(now),
                                                                sw::redis::BoundType::CLOSED),
                             std::back_inserter(members));
      } catch (const sw::redis::Error& e) {
        redis_error("ZRANGEBYSCORE", e);
        return cb(error(drogon::k503ServiceUnavailable, "Redis 不可用"));
      }
      std::vector<lm::Sample> samples;
      samples.reserve(members.size());
      google::protobuf::util::JsonParseOptions po;
      po.ignore_unknown_fields = true;
      for (const auto& m : members) {
        lm::Sample s;
        if (google::protobuf::util::JsonStringToMessage(m, &s, po).ok()) samples.push_back(std::move(s));
      }
      cb(json(series_json(id, step_s * 1000, downsample(samples, step_s * 1000))));
    }, {Get});

    auto events = [this](const HttpRequestPtr& req, Callback&& cb, const std::string& node) {
      if (!authorized(req, false)) return cb(error(drogon::k401Unauthorized, "unauthorized"));
      const long long limit = int_param(req, "limit", node.empty() ? 200 : 100, 1, 1000);
      std::vector<Item> items;
      try {
        redis->xrevrange(key("stream:events"), "+", "-", node.empty() ? limit : opt.backfill_max,
                         std::back_inserter(items));
      } catch (const sw::redis::Error& e) {
        redis_error("XREVRANGE", e);
        return cb(error(drogon::k503ServiceUnavailable, "Redis 不可用"));
      }
      std::string arr = "[";
      long long n = 0;
      for (const auto& [id, f] : items) {
        if (!node.empty() && field_of(f, "node") != node) continue;
        if (n++ > 0) arr += ',';
        arr += event_json(id, f, false);
        if (n >= limit) break;
      }
      arr += "]";
      cb(json(JsonObj().raw("events", arr).str()));
    };
    app.registerHandler("/api/events", [events](const HttpRequestPtr& req, Callback&& cb) {
      events(req, std::move(cb), "");
    }, {Get});
    app.registerHandler("/api/nodes/{id}/events", [events](const HttpRequestPtr& req, Callback&& cb,
                                                           const std::string& id) {
      events(req, std::move(cb), id);
    }, {Get});
  }
};

namespace {

// WebSocket /ws：转给 WebServer::Impl
class LiveWs : public drogon::WebSocketController<LiveWs, false> {
 public:
  explicit LiveWs(WebServer::Impl* impl) : impl_(impl) {}
  void handleNewMessage(const WebSocketConnectionPtr&, std::string&&,
                        const drogon::WebSocketMessageType&) override {}  // 客户端只收不发
  void handleNewConnection(const HttpRequestPtr& req, const WebSocketConnectionPtr& conn) override {
    impl_->on_open(req, conn);
  }
  void handleConnectionClosed(const WebSocketConnectionPtr& conn) override { impl_->on_close(conn); }

  WS_PATH_LIST_BEGIN
  WS_PATH_ADD("/ws");
  WS_PATH_LIST_END

 private:
  WebServer::Impl* impl_;
};

}  // namespace

bool WebServer::compiled_in() { return true; }

WebServer::WebServer(NodeTable& table, Options opt) : impl_(std::make_unique<Impl>(table, std::move(opt))) {}
WebServer::~WebServer() { stop(); }

std::size_t WebServer::connections() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->conns.size();
}

void WebServer::start() {
  Impl& m = *impl_;
  if (!m.opt.enabled || m.running.exchange(true)) return;
  if (m.opt.token.empty()) spdlog::warn("看板接口未设置 token（api.token / LM_API_TOKEN），不鉴权");
  m.redis = std::make_unique<sw::redis::Redis>(m.conn_opts(std::chrono::milliseconds(500)));
  m.routes();
  auto& app = drogon::app();
  app.registerController(std::make_shared<LiveWs>(impl_.get()));
  app.setLogLevel(trantor::Logger::kWarn)
      .addListener(m.opt.host, m.opt.port)
      .setThreadNum(m.opt.threads)
      .disableSigtermHandling();
  m.app_thread = std::thread([] { drogon::app().run(); });
  m.pump_thread = std::thread([&m] { m.pump(); });
  m.push_thread = std::thread([&m] { m.push(); });
  spdlog::info("看板接口监听 {}:{}", m.opt.host, m.opt.port);
}

void WebServer::stop() {
  Impl& m = *impl_;
  if (!m.running.exchange(false)) return;
  // run() 还没进入事件循环时 quit 会被忽略：等它跑起来再退
  for (int i = 0; i < 50 && !drogon::app().isRunning(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  drogon::app().quit();
  if (m.app_thread.joinable()) m.app_thread.join();
  if (m.pump_thread.joinable()) m.pump_thread.join();
  if (m.push_thread.joinable()) m.push_thread.join();
}

}  // namespace lm::manager

#else  // 桩

namespace lm::manager {

struct WebServer::Impl {
  Options opt;
};

bool WebServer::compiled_in() { return false; }
WebServer::WebServer(NodeTable&, Options opt) : impl_(std::make_unique<Impl>(Impl{std::move(opt)})) {}
WebServer::~WebServer() = default;
std::size_t WebServer::connections() const { return 0; }
void WebServer::start() {
  if (impl_->opt.enabled) spdlog::warn("看板接口：未编译 Drogon 支持，不启动");
}
void WebServer::stop() {}

}  // namespace lm::manager

#endif
