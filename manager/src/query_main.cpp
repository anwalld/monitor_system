#include <charconv>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

#include <google/protobuf/util/time_util.h>

#include "rpc/rpc_client.h"

namespace {

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program
            << " <manager_address> <server_name> <start_time> <end_time>"
               " [page] [page_size]\n"
            << "  Times must be RFC3339, e.g. 2026-09-17T00:00:00+08:00\n"
            << "  page defaults to 1; page_size defaults to 100.\n";
}

bool ParsePositiveInt(const std::string& text, int* value) {
  auto result = std::from_chars(text.data(), text.data() + text.size(), *value);
  return result.ec == std::errc() && result.ptr == text.data() + text.size() &&
         *value > 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 &&
      (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (argc < 5 || argc > 7) {
    PrintUsage(argv[0]);
    return 2;
  }
  if (std::string(argv[1]).empty() || std::string(argv[2]).empty()) {
    std::cerr << "Manager address and server name must not be empty.\n";
    return 2;
  }

  monitor::proto::QueryPerformanceRequest request;
  request.set_server_name(argv[2]);
  auto* range = request.mutable_time_range();
  using google::protobuf::util::TimeUtil;
  if (!TimeUtil::FromString(argv[3], range->mutable_start_time()) ||
      !TimeUtil::FromString(argv[4], range->mutable_end_time())) {
    std::cerr << "Invalid time: use RFC3339 with a timezone (Z or +08:00).\n";
    return 2;
  }
  const auto& start = range->start_time();
  const auto& end = range->end_time();
  if (start.seconds() > end.seconds() ||
      (start.seconds() == end.seconds() && start.nanos() > end.nanos())) {
    std::cerr << "Start time must not be later than end time.\n";
    return 2;
  }

  int page = 1;
  int page_size = 100;
  if ((argc > 5 && !ParsePositiveInt(argv[5], &page)) ||
      (argc > 6 && !ParsePositiveInt(argv[6], &page_size)) ||
      page - 1 > std::numeric_limits<int>::max() / page_size) {
    std::cerr << "Invalid pagination: use positive integers with an offset "
                 "within the int32 range.\n";
    return 2;
  }
  request.mutable_pagination()->set_page(page);
  request.mutable_pagination()->set_page_size(page_size);

  monitor::RpcClient client(argv[1]);
  monitor::proto::QueryPerformanceResponse response;
  if (!client.QueryPerformance(request, &response)) {
    return 1;
  }

  std::cout << "Total: " << response.total_count()
            << ", page: " << response.page()
            << ", page size: " << response.page_size() << '\n';
  if (response.records().empty()) {
    std::cout << "No records returned.\n";
    return 0;
  }
  std::cout << "Timestamp (UTC)\tServer\tCPU (%)\tMemory (%)\tDisk (%)"
               "\tLoad (1m)\tScore\n"
            << std::fixed << std::setprecision(2);
  for (const auto& record : response.records()) {
    std::cout << TimeUtil::ToString(record.timestamp()) << '\t'
              << record.server_name() << '\t' << record.cpu_percent() << '\t'
              << record.mem_used_percent() << '\t' << record.disk_util_percent()
              << '\t' << record.load_avg_1() << '\t' << record.score() << '\n';
  }
  return 0;
}
