#pragma once

#include <cstdint>
#include <string>

namespace random_name {

// sex: 1=男 2=女；其它返回昵称池
std::string Generate(uint32_t sex);

}  // namespace random_name
