#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ecs/component_base/component_base.h"

// 角色列表项；选角展示与 MarkLast，不单独作为组件。
struct RoleInfo {
  uint64_t role_id = 0;
  std::string name;
  uint32_t sex  = 0;
  uint32_t job  = 0;
  uint32_t level = 1;
  uint64_t create_time = 0;
  bool is_last = false;
};

// 当前选中角色 + all_roles_ 列表。选角 SetCurrent；升级/改名须同步两处。
class RoleComponent : public IComponent {
 public:
  ComponentType Type() const override { return ComponentType::kRole; }

  uint64_t role_id_ = 0;
  std::string name_;
  uint32_t sex_  = 0;
  uint32_t job_  = 0;
  uint32_t level_ = 1;
  uint64_t create_time_ = 0;
  bool is_last_ = false;

  std::vector<RoleInfo> all_roles_;

  void SetCurrent(const RoleInfo& info)
  {
    role_id_     = info.role_id;
    name_        = info.name;
    sex_         = info.sex;
    job_          = info.job;
    level_       = info.level;
    create_time_ = info.create_time;
    is_last_     = info.is_last;
  }

  void MarkLast(uint64_t role_id)
  {
    for (auto& r : all_roles_)
    {
      r.is_last = (r.role_id == role_id);
    }
  }
};

DECLARE_COMPONENT(RoleComponent, ComponentType::kRole)
