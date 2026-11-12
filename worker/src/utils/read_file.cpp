#include "utils/read_file.h"

namespace monitor {
bool ReadFile::ReadLine(std::vector<std::string>* args) {
  std::string line;
  std::getline(ifs_, line); //从文件流读取一行
  if (ifs_.eof() || line.empty()) {   //文件结束或空行
    return false;
  }

  //将读取到的行转换为字符串流
  std::istringstream line_ss(line);
  while (!line_ss.eof()) {  
    std::string word;
    line_ss >> word;  //按空格分割字符串
    args->push_back(word);//将每个单词存入传入的args向量中。
  }
  return true;
}


}  // namespace monitor