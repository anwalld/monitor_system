#pragma once

#include <boost/chrono.hpp>

//用于精确计算两个时间点之间的时间差（以秒为单位）
namespace monitor {
class Utils {
 public:
 //静态方法，可以直接通过类名调用，无需创建类的实例
 //duration对象表示一段时间间隔
 //chrono 库中提供了一个表示时间点的类 time_point
  static double SteadyTimeSecond(
      const boost::chrono::steady_clock::time_point &t1,
      const boost::chrono::steady_clock::time_point &t2) {
    boost::chrono::duration<double> time_second = t1 - t2;
    return time_second.count(); //count()是duration类的方法，返回时间间隔的数值部分
  }
};
}  // namespace monitor