#pragma once

// 评分：归一化、网络分、加权总分。纯函数，调用方负责加锁。

#include <string>

#include "monitor.pb.h"
#include "scoring_params.h"

namespace lm::manager {

struct ScoreBreakdown {
  double cpu = 0;
  double runq = 0;
  double mem = 0;
  double net = 0;
  double total = 0;
  std::string best_nic;       // 网络分来自哪块网卡；没有网卡时为空
  bool runq_missing = false;  // 没有上报调度延迟，按 missing_score 计
  bool net_missing = false;   // nics 为空，按 missing_score 计
  int invalid = 0;            // 非有限输入的个数（按 0 分处理）
};

// s = clamp((bad − x) / (bad − good), 0, 1)；x 非有限时返回 0。
double normalize(double x, const Range& r);
// 先取自然对数再映射；x ≤ 0 时按好点处理；x 非有限时返回 0。要求 good、bad > 0。
double normalize_log(double x, const Range& r);
// 一块网卡的分数：RTT、超时率、重传率三项分数的最小值。
double nic_score(const lm::NicStat& nic, const NetParams& p, int* invalid = nullptr);

ScoreBreakdown compute_score(const lm::Sample& s, const ScoringParams& p);

}  // namespace lm::manager
