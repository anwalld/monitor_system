// lm-bench：压测与调试客户端。
//   lm-bench allocate [-m 127.0.0.1:50051] [-n 1] [-c bench]
//     调 n 次 Scheduler.Allocate，n=1 时打印 task_id 与地址，否则打印每个地址被选中的次数。
//   lm-bench submit [-m 127.0.0.1:50051] [-r 5] [-d 60] [-s 1] [-c bench] [--cmd /bin/sleep]
//                   [--args "0.2"] [--bytes 1024] [--token T] [--timeout 30] [--csv out.csv] [--print]
//     按泊松到达（速率 r，固定种子）执行 Allocate → MarkSent → 直连 Worker Submit，
//     每秒打印各 Worker 的分配占比，结束时打印完成时间分位数与失败分类。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "task.grpc.pb.h"

namespace {

using Clock = std::chrono::steady_clock;

void usage() {
  std::cerr << "用法:\n"
               "  lm-bench allocate [-m manager] [-n 次数] [-c client_id]\n"
               "  lm-bench submit [-m manager] [-r 每秒任务数] [-d 秒数] [-s 种子] [-c client_id]\n"
               "                  [--cmd 命令] [--args \"参数\"] [--bytes 输入字节] [--token T]\n"
               "                  [--timeout 秒] [--csv 文件] [--print]\n";
}

int cmd_allocate(int argc, char** argv) {
  std::string mgr = "127.0.0.1:50051";
  std::string client = "bench";
  long n = 1;
  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-m" && i + 1 < argc) {
      mgr = argv[++i];
    } else if (a == "-n" && i + 1 < argc) {
      n = std::strtol(argv[++i], nullptr, 10);
    } else if (a == "-c" && i + 1 < argc) {
      client = argv[++i];
    } else {
      usage();
      return 2;
    }
  }
  auto stub = lm::Scheduler::NewStub(grpc::CreateChannel(mgr, grpc::InsecureChannelCredentials()));
  std::map<std::string, long> hits;
  std::map<std::string, long> errors;
  for (long i = 0; i < n; ++i) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    lm::AllocateReq req;
    req.set_client_id(client);
    lm::AllocateResp resp;
    const grpc::Status st = stub->Allocate(&ctx, req, &resp);
    if (n == 1) {
      if (!st.ok()) {
        std::cout << "错误 " << st.error_code() << ": " << st.error_message() << "\n";
        return 1;
      }
      std::cout << resp.task_id() << " " << resp.worker_addr() << "\n";
      return 0;
    }
    if (st.ok()) {
      ++hits[resp.worker_addr()];
    } else {
      ++errors[std::to_string(st.error_code()) + " " + st.error_message()];
    }
  }
  for (const auto& [addr, c] : hits) std::printf("%-24s %8ld  %.4f\n", addr.c_str(), c, double(c) / n);
  for (const auto& [e, c] : errors) std::printf("错误 %s  ×%ld\n", e.c_str(), c);
  return errors.empty() ? 0 : 1;
}

struct SubmitOpts {
  std::string mgr = "127.0.0.1:50051";
  double rate = 5;
  double duration_s = 60;
  uint64_t seed = 1;
  std::string client = "bench";
  std::string cmd = "/bin/sleep";
  std::vector<std::string> args = {"0.2"};
  uint64_t bytes = 1024;
  std::string token;
  uint32_t timeout_s = 30;
  std::string csv;
  bool print = false;  // 打印每个任务的输出末尾（调试用）
};

struct TaskRecord {
  int64_t start_ms = 0;  // 相对开始
  std::string task_id;
  std::string addr;
  std::string stage = "allocate";  // 出错的阶段；成功为 done
  int code = 0;                    // gRPC 状态码
  int exit_code = 0;
  int64_t latency_ms = 0;
};

class Bench {
 public:
  explicit Bench(SubmitOpts o)
      : o_(std::move(o)),
        sched_(lm::Scheduler::NewStub(grpc::CreateChannel(o_.mgr, grpc::InsecureChannelCredentials()))) {}

  int run();

 private:
  lm::WorkerTask::Stub* worker(const std::string& addr) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& s = workers_[addr];
    if (!s) s = lm::WorkerTask::NewStub(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
    return s.get();
  }
  void one(int64_t start_ms);
  void print_second(int sec);

  SubmitOpts o_;
  std::unique_ptr<lm::Scheduler::Stub> sched_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, std::unique_ptr<lm::WorkerTask::Stub>> workers_;
  std::vector<TaskRecord> records_;
  std::map<std::string, long> sec_hits_;  // 本秒各地址的分配数
  std::map<std::string, long> total_hits_;
  long done_ = 0;
  long failed_ = 0;
  int active_ = 0;
  Clock::time_point t0_;
};

void Bench::one(int64_t start_ms) {
  TaskRecord rec;
  rec.start_ms = start_ms;
  const auto begin = Clock::now();
  auto finish = [&](const std::string& stage, const grpc::Status& st) {
    rec.stage = stage;
    rec.code = st.error_code();
    rec.latency_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
  };
  do {
    lm::AllocateReq areq;
    areq.set_client_id(o_.client);
    areq.set_input_bytes(o_.bytes);
    lm::AllocateResp aresp;
    grpc::ClientContext actx;
    actx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    grpc::Status st = sched_->Allocate(&actx, areq, &aresp);
    if (!st.ok()) {
      finish("allocate", st);
      break;
    }
    rec.task_id = aresp.task_id();
    rec.addr = aresp.worker_addr();
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++sec_hits_[rec.addr];
      ++total_hits_[rec.addr];
    }
    lm::TaskRef ref;
    ref.set_task_id(rec.task_id);
    lm::Ack ack;
    grpc::ClientContext mctx;
    mctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    st = sched_->MarkSent(&mctx, ref, &ack);
    if (!st.ok()) {
      finish("marksent", st);
      break;
    }
    lm::TaskReq treq;
    treq.set_task_id(rec.task_id);
    treq.set_command(o_.cmd);
    for (const auto& a : o_.args) treq.add_args(a);
    treq.set_timeout_s(o_.timeout_s);
    lm::TaskResult tres;
    grpc::ClientContext tctx;
    tctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(o_.timeout_s + 5));
    if (!o_.token.empty()) tctx.AddMetadata("authorization", "Bearer " + o_.token);
    st = worker(rec.addr)->Submit(&tctx, treq, &tres);
    if (!st.ok()) {
      finish("submit", st);
      break;
    }
    rec.exit_code = tres.exit_code();
    if (o_.print) {
      std::lock_guard<std::mutex> lk(mu_);
      std::printf("[%s @ %s] exit=%d exec=%ums\n%s", rec.task_id.c_str(), rec.addr.c_str(),
                  tres.exit_code(), tres.exec_ms(), tres.stdout_tail().c_str());
    }
    finish(tres.exit_code() == 0 ? "done" : "exit", st);
  } while (false);

  std::lock_guard<std::mutex> lk(mu_);
  (rec.stage == "done" ? done_ : failed_) += 1;
  records_.push_back(std::move(rec));
  --active_;
  cv_.notify_all();
}

void Bench::print_second(int sec) {
  std::lock_guard<std::mutex> lk(mu_);
  long n = 0;
  for (const auto& [a, c] : sec_hits_) n += c;
  std::printf("t=%3ds  分配 %3ld", sec, n);
  for (const auto& [a, c] : total_hits_) {
    const long v = sec_hits_.count(a) ? sec_hits_[a] : 0;
    std::printf("  %s %3ld(%3.0f%%)", a.c_str(), v, n > 0 ? 100.0 * v / n : 0.0);
  }
  std::printf("  | 完成 %ld 失败 %ld 进行中 %d\n", done_, failed_, active_);
  std::fflush(stdout);
  sec_hits_.clear();
}

int Bench::run() {
  std::mt19937_64 rng(o_.seed);
  std::exponential_distribution<double> gap(o_.rate);
  // 先按种子生成全部到达时刻，保证同一种子的到达序列完全相同
  std::vector<double> arrivals;
  for (double t = gap(rng); t < o_.duration_s; t += gap(rng)) arrivals.push_back(t);
  std::printf("计划提交 %zu 个任务（种子 %llu，速率 %.1f/s，%.0f 秒）\n", arrivals.size(),
              static_cast<unsigned long long>(o_.seed), o_.rate, o_.duration_s);

  t0_ = Clock::now();
  std::vector<std::thread> threads;
  int next_print = 1;
  constexpr int kMaxActive = 256;
  for (double at : arrivals) {
    const auto when = t0_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(at));
    while (Clock::now() < when) {
      const auto tick = t0_ + std::chrono::seconds(next_print);
      if (tick <= when && Clock::now() >= tick) {
        print_second(next_print++);
        continue;
      }
      std::this_thread::sleep_until(std::min(when, tick));
    }
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return active_ < kMaxActive; });
      ++active_;
    }
    const int64_t start_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count();
    threads.emplace_back([this, start_ms] { one(start_ms); });
  }
  while (Clock::now() < t0_ + std::chrono::duration<double>(o_.duration_s)) {
    std::this_thread::sleep_until(t0_ + std::chrono::seconds(next_print));
    print_second(next_print++);
  }
  for (auto& t : threads) t.join();
  print_second(next_print);

  // 汇总
  std::vector<int64_t> lat;
  std::map<std::string, long> fails;
  for (const auto& r : records_) {
    if (r.stage == "done") {
      lat.push_back(r.latency_ms);
    } else {
      fails[r.stage + " code=" + std::to_string(r.code) +
            (r.stage == "exit" ? " exit=" + std::to_string(r.exit_code) : "")]++;
    }
  }
  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) -> int64_t {
    if (lat.empty()) return 0;
    const std::size_t i = std::min(lat.size() - 1, static_cast<std::size_t>(p * (lat.size() - 1) + 0.5));
    return lat[i];
  };
  std::printf("\n== 汇总 ==\n任务 %zu，完成 %zu，失败 %zu\n", records_.size(), lat.size(),
              records_.size() - lat.size());
  std::printf("完成时间 ms：P50 %lld  P99 %lld  最大 %lld\n", static_cast<long long>(pct(0.5)),
              static_cast<long long>(pct(0.99)), static_cast<long long>(lat.empty() ? 0 : lat.back()));
  long hits = 0;
  for (const auto& [a, c] : total_hits_) hits += c;
  for (const auto& [a, c] : total_hits_) {
    std::printf("  %-24s %6ld  %.4f\n", a.c_str(), c, hits > 0 ? double(c) / hits : 0.0);
  }
  for (const auto& [k, c] : fails) std::printf("失败 %s ×%ld\n", k.c_str(), c);

  if (!o_.csv.empty()) {
    std::ofstream f(o_.csv);
    f << "start_ms,task_id,worker_addr,stage,code,exit_code,latency_ms\n";
    std::vector<TaskRecord> sorted = records_;
    std::sort(sorted.begin(), sorted.end(),
              [](const TaskRecord& a, const TaskRecord& b) { return a.start_ms < b.start_ms; });
    for (const auto& r : sorted) {
      f << r.start_ms << ',' << r.task_id << ',' << r.addr << ',' << r.stage << ',' << r.code << ','
        << r.exit_code << ',' << r.latency_ms << '\n';
    }
    std::printf("明细已写入 %s\n", o_.csv.c_str());
  }
  return records_.size() == lat.size() ? 0 : 1;
}

int cmd_submit(int argc, char** argv) {
  SubmitOpts o;
  for (int i = 0; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-m") {
      o.mgr = next();
    } else if (a == "-r") {
      o.rate = std::stod(next());
    } else if (a == "-d") {
      o.duration_s = std::stod(next());
    } else if (a == "-s") {
      o.seed = std::stoull(next());
    } else if (a == "-c") {
      o.client = next();
    } else if (a == "--cmd") {
      o.cmd = next();
    } else if (a == "--args") {
      o.args.clear();
      std::istringstream ss(next());
      for (std::string w; ss >> w;) o.args.push_back(w);
    } else if (a == "--bytes") {
      o.bytes = std::stoull(next());
    } else if (a == "--token") {
      o.token = next();
    } else if (a == "--timeout") {
      o.timeout_s = static_cast<uint32_t>(std::stoul(next()));
    } else if (a == "--csv") {
      o.csv = next();
    } else if (a == "--print") {
      o.print = true;
    } else {
      usage();
      return 2;
    }
  }
  if (!(o.rate > 0) || !(o.duration_s > 0)) {
    usage();
    return 2;
  }
  return Bench(o).run();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "allocate") return cmd_allocate(argc - 2, argv + 2);
  if (cmd == "submit") return cmd_submit(argc - 2, argv + 2);
  usage();
  return 2;
}
