#include "mysql_client.h"

#ifdef LM_HAVE_MYSQL
#include LM_MYSQL_HEADER
#endif

namespace lm::manager {

namespace {

// 未连接时的保守转义（与 mysql_real_escape_string 在 utf8mb4 下等价的字符集合）。
// mariadb-connector-c 的 mysql_escape_string 在没有连接时会崩溃，不能用。
std::string escape_plain(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\0': out += "\\0"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\x1a': out += "\\Z"; break;
      case '\'': out += "\\'"; break;
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

MysqlClient::MysqlClient(MysqlOptions opt) : opt_(std::move(opt)) {}
MysqlClient::~MysqlClient() { close(); }

#ifdef LM_HAVE_MYSQL

bool MysqlClient::compiled_in() { return true; }

void MysqlClient::close() {
  if (conn_ != nullptr) {
    mysql_close(static_cast<MYSQL*>(conn_));
    conn_ = nullptr;
  }
}

bool MysqlClient::ensure(int64_t now_ms) {
  if (conn_ != nullptr) return true;
  if (last_attempt_ms_ >= 0 && now_ms - last_attempt_ms_ < opt_.reconnect_interval_ms) return false;
  last_attempt_ms_ = now_ms;
  MYSQL* m = mysql_init(nullptr);
  if (m == nullptr) {
    error_ = "mysql_init 失败";
    return false;
  }
  unsigned t = opt_.timeout_s;
  mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
  mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &t);
  mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &t);
  if (mysql_real_connect(m, opt_.host.c_str(), opt_.user.c_str(), opt_.password.c_str(),
                         opt_.database.c_str(), opt_.port, nullptr, 0) == nullptr) {
    error_ = mysql_error(m);
    mysql_close(m);
    return false;
  }
  mysql_set_character_set(m, "utf8mb4");
  // 时间列按 UTC 写入，与样本时间戳一致
  mysql_query(m, "SET time_zone = '+00:00'");
  conn_ = m;
  error_.clear();
  return true;
}

bool MysqlClient::exec(const std::string& sql) {
  if (conn_ == nullptr) {
    error_ = "未连接";
    return false;
  }
  auto* m = static_cast<MYSQL*>(conn_);
  if (mysql_real_query(m, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    error_ = mysql_error(m);
    const unsigned err = mysql_errno(m);
    // 2000–2999 是客户端/连接错误：断开，下次重连
    if (err >= 2000 && err < 3000) close();
    return false;
  }
  // 消费可能的结果集
  if (MYSQL_RES* r = mysql_store_result(m)) mysql_free_result(r);
  return true;
}

std::optional<std::vector<std::vector<std::string>>> MysqlClient::query(const std::string& sql) {
  if (conn_ == nullptr) {
    error_ = "未连接";
    return std::nullopt;
  }
  auto* m = static_cast<MYSQL*>(conn_);
  if (mysql_real_query(m, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    error_ = mysql_error(m);
    if (mysql_errno(m) >= 2000 && mysql_errno(m) < 3000) close();
    return std::nullopt;
  }
  std::vector<std::vector<std::string>> rows;
  MYSQL_RES* r = mysql_store_result(m);
  if (r == nullptr) return rows;
  const unsigned nf = mysql_num_fields(r);
  while (MYSQL_ROW row = mysql_fetch_row(r)) {
    unsigned long* lens = mysql_fetch_lengths(r);
    std::vector<std::string> v;
    for (unsigned i = 0; i < nf; ++i) v.emplace_back(row[i] != nullptr ? std::string(row[i], lens[i]) : "");
    rows.push_back(std::move(v));
  }
  mysql_free_result(r);
  return rows;
}

std::string MysqlClient::escape(const std::string& s) const {
  if (conn_ == nullptr) return escape_plain(s);
  std::string out(s.size() * 2 + 1, '\0');
  const unsigned long n = mysql_real_escape_string(static_cast<MYSQL*>(conn_), out.data(), s.data(),
                                                   static_cast<unsigned long>(s.size()));
  out.resize(n);
  return out;
}

#else  // 桩

bool MysqlClient::compiled_in() { return false; }
void MysqlClient::close() {}
bool MysqlClient::ensure(int64_t) {
  error_ = "未编译 MySQL 支持（构建时没有找到 MySQL 客户端库）";
  return false;
}
bool MysqlClient::exec(const std::string&) {
  error_ = "未编译 MySQL 支持";
  return false;
}
std::optional<std::vector<std::vector<std::string>>> MysqlClient::query(const std::string&) {
  error_ = "未编译 MySQL 支持";
  return std::nullopt;
}
std::string MysqlClient::escape(const std::string& s) const { return escape_plain(s); }

#endif

}  // namespace lm::manager
