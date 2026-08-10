#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>

struct MapConfig {
  uint32_t    cfg_id_      = 0;
  std::string name_;
  std::string res_id_;
  uint32_t    dist_flag_   = 0;
  uint32_t    map_type_    = 0;
  JPH::Vec3   born_pos_    = JPH::Vec3::sZero();
  JPH::Quat   born_rot_    = JPH::Quat::sIdentity();
  JPH::Quat   born_move_rot_ = JPH::Quat::sIdentity();
  float       born_range_  = 3.0f;
};

class MapConfigSystem {
 public:
  static MapConfigSystem& Instance() {
    static MapConfigSystem inst;
    return inst;
  }

  void LoadDefaults();
  void AddConfig(const MapConfig& cfg) {
    configs_[cfg.cfg_id_] = cfg;
    ordered_.push_back(cfg.cfg_id_);
  }
  const MapConfig* Find(uint32_t cfg_id) const {
    auto it = configs_.find(cfg_id);
    if (it == configs_.end()) return nullptr;
    return &it->second;
  }
  const MapConfig* GetFirstMap() const {
    if (ordered_.empty()) return nullptr;
    return Find(ordered_.front());
  }
  const std::vector<uint32_t>& GetAllCfgIds() const { return ordered_; }

 private:
  MapConfigSystem() = default;
  std::unordered_map<uint32_t, MapConfig> configs_;
  std::vector<uint32_t> ordered_;
};
