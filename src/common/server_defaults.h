#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace server {

// 监听默认值（可用 argv / 环境变量覆盖）
inline constexpr const char* kDefaultListenIp = "0.0.0.0";
inline constexpr int kDefaultListenPort = 20002;

// ID 生成起点
inline constexpr uint64_t kSessionIdStart = 10001;
inline constexpr uint64_t kRoleIdStart = 20001;

// 从项目根目录或 bin/ 目录启动时都能定位到地图资源。
inline constexpr const char* kDefaultMapResDir = "deps/map_res/";

// 拼 OBJ 路径：优先 GAME_MAP_OBJ；否则用 MapConfig.res_id_
inline std::string ResolveMapObjPath(const std::string& res_id) {
  if (const char* env = std::getenv("GAME_MAP_OBJ")) {
    if (env[0] != '\0') {
      return std::string(env);
    }
  }
  if (res_id.empty()) {
    return std::string(kDefaultMapResDir) + "1001.obj";
  }
  const std::string filename = res_id + ".obj";
  const std::filesystem::path candidates[] = {
      std::filesystem::path(kDefaultMapResDir) / filename,
      std::filesystem::path("../deps/map_res") / filename,
  };
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error) && !error) {
      return candidate.string();
    }
    error.clear();
  }
  return candidates[0].string();
}

}  // namespace server
