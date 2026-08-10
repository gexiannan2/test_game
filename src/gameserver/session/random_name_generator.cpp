#include "session/random_name_generator.h"

#include <random>
#include <vector>

namespace random_name {

std::string Generate(uint32_t sex) {
  static const std::vector<std::string> kLastNames = {
      "李", "王", "张", "刘", "陈", "杨", "赵", "黄", "周", "吴",
      "徐", "孙", "胡", "朱", "高", "林", "何", "郭", "马", "罗"};
  static const std::vector<std::string> kMaleNames = {
      "伟", "强", "磊", "军", "杰", "勇", "涛", "明", "超", "峰",
      "辉", "鹏", "斌", "宇", "浩", "然", "轩", "瀚", "辰", "泽"};
  static const std::vector<std::string> kFemaleNames = {
      "芳", "娜", "敏", "静", "丽", "娟", "婷", "雪", "倩", "颖",
      "萍", "琳", "晓", "燕", "雯", "蓉", "蕊", "瑶", "玲", "妮"};
  static const std::vector<std::string> kNickNames = {
      "逍遥", "独孤", "无名", "听雨", "落霞",
      "清风", "明月", "孤星", "残月", "暗影"};

  thread_local std::mt19937 gen(std::random_device{}());

  if (sex != 1 && sex != 2) {
    if (!kNickNames.empty()) {
      return kNickNames[gen() % kNickNames.size()];
    }
    return "";
  }

  if (!kNickNames.empty() && (gen() % 2 == 0)) {
    return kNickNames[gen() % kNickNames.size()];
  }

  const std::string& last = kLastNames[gen() % kLastNames.size()];
  if (sex == 1) {
    return last + kMaleNames[gen() % kMaleNames.size()];
  }
  return last + kFemaleNames[gen() % kFemaleNames.size()];
}

}  // namespace random_name
