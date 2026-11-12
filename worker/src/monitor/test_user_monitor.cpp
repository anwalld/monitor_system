/**
 * 用户名信息采集测试程序
 * 
 * 编译: g++ -o test_user_monitor test_user_monitor.cpp -std=c++11
 * 运行: ./test_user_monitor
 */

#include <unistd.h>
#include <sys/types.h>

#include <fstream>
#include <sstream>
#include <string>
#include <iostream>

/**
 * 根据 UID 从 /etc/passwd 文件中查找用户名
 * 模拟 getpwuid() 的核心功能
 */
std::string GetUsernameByUid(uid_t uid) {
  std::ifstream passwd_file("/etc/passwd");
  if (!passwd_file.is_open()) {
    std::cerr << "Failed to open /etc/passwd" << std::endl;
    return "";
  }

  std::string line;
  while (std::getline(passwd_file, line)) {
    // /etc/passwd 格式: username:password:uid:gid:gecos:home:shell
    std::istringstream iss(line);
    std::string username, password, uid_str;

    // 解析第一个字段：用户名
    if (!std::getline(iss, username, ':')) {
      continue;
    }

    // 解析第二个字段：密码（通常是 x）
    if (!std::getline(iss, password, ':')) {
      continue;
    }

    // 解析第三个字段：UID
    if (!std::getline(iss, uid_str, ':')) {
      continue;
    }

    // 将 UID 字符串转换为数字并比较
    try {
      uid_t parsed_uid = static_cast<uid_t>(std::stoul(uid_str));
      if (parsed_uid == uid) {
        return username;
      }
    } catch (const std::exception& e) {
      continue;
    }
  }

  return "";
}

int main() {
  // 使用系统调用获取当前进程的实际用户ID
  uid_t uid = getuid();
  std::cout << "Current UID: " << uid << std::endl;

  // 根据 UID 查找用户名
  std::string username = GetUsernameByUid(uid);

  if (!username.empty()) {
    std::cout << "Username: " << username << std::endl;
  } else {
    std::cout << "Failed to find username for UID " << uid << std::endl;
  }

  // 对比环境变量方式
  const char* env_user = getenv("USER");
  if (env_user) {
    std::cout << "Environment USER: " << env_user << std::endl;
  } else {
    std::cout << "Environment USER not set" << std::endl;
  }

  return 0;
}
