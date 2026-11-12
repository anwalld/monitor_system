#pragma once

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace monitor {
class ReadFile {
 public:
 //接受文件名作为参数，并初始化文件输入流ifs_
  explicit ReadFile(const std::string& name) : ifs_(name) {}
  ~ReadFile() { ifs_.close(); }

  bool ReadLine(std::vector<std::string>* args);
  // static std::vector<std::string> GetStatsLines(const std::string& stat_file,
  //                                               const int line_count) {
  //   std::vector<std::string> stats_lines;
  //   std::ifstream buffer(stat_file);
  //   for (int line_num = 0; line_num < line_count; ++line_num) {
  //     std::string line;
  //     std::getline(buffer, line);
  //     if (line.empty()) {
  //       break;
  //     }
  //     stats_lines.push_back(line);
  //   }
  //   return stats_lines;
  // }

 private:
  std::ifstream ifs_;  //定义了一个私有的文件输入流对象，用于实际的文件读取操作
};
}  // namespace monitor
