#pragma once

#include <Jolt/Jolt.h>

#include <cmath>
#include <string>

#include "client_common.pb.h"  // ::vec3, ::quat

// Transform 相关通用工具：数值有限性校验、protobuf→Jolt 提取、日志格式化。
// 纯数学/转换，无业务依赖，可供 handlers / ecs / tests 等任意模块复用。

namespace game_util {

// ---- 数值有限性校验 ----

inline bool Vec3Finite(const JPH::Vec3& v) {
    return std::isfinite(v.GetX()) && std::isfinite(v.GetY()) &&
           std::isfinite(v.GetZ());
}

inline bool QuatFinite(const JPH::Quat& q) {
    return std::isfinite(q.GetX()) && std::isfinite(q.GetY()) &&
           std::isfinite(q.GetZ()) && std::isfinite(q.GetW());
}

// ---- protobuf → Jolt 提取 ----

inline JPH::Vec3 ExtractVec3(const ::vec3& v) {
    return JPH::Vec3(v.x(), v.y(), v.z());
}

inline JPH::Quat ExtractQuat(const ::quat& q) {
    return JPH::Quat(q.x(), q.y(), q.z(), q.w());
}

// ---- 日志格式化 ----

inline std::string FormatVec3(const JPH::Vec3& v) {
    return "(" + std::to_string(v.GetX()) + "," + std::to_string(v.GetY()) + "," +
           std::to_string(v.GetZ()) + ")";
}

inline std::string FormatQuat(const JPH::Quat& q) {
    return "(" + std::to_string(q.GetX()) + "," + std::to_string(q.GetY()) + "," +
           std::to_string(q.GetZ()) + "," + std::to_string(q.GetW()) + ")";
}

}  // namespace game_util
