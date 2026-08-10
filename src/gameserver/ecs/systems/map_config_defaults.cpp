// 地图 demo 静态默认表（正式环境可换读表）。

#include "ecs/systems/map_config_system.h"

#include "zrpc/base/logger.h"

namespace {

struct MapConfigDefault {
  uint32_t cfg_id;
  const char* name;
  const char* res_id;
  float born_x;
  float born_y;
  float born_z;
  float born_range;
};

constexpr MapConfigDefault kMapConfigDefaults[] = {
    {1001, "新手村", "1001", 333.0f, 18.0f, 415.45f, 3.0f},
    {1201, "野外森林", "1201", 333.80f, 4.36f, 303.43f, 3.0f},
};

}  // namespace

void MapConfigSystem::LoadDefaults() {
  for (const auto& d : kMapConfigDefaults) {
    MapConfig cfg;
    cfg.cfg_id_ = d.cfg_id;
    cfg.name_ = d.name;
    cfg.res_id_ = d.res_id;
    cfg.dist_flag_ = 0;
    cfg.map_type_ = 0;
    cfg.born_pos_ = JPH::Vec3(d.born_x, d.born_y, d.born_z);
    cfg.born_rot_ = JPH::Quat::sIdentity();
    cfg.born_move_rot_ = JPH::Quat::sIdentity();
    cfg.born_range_ = d.born_range;
    AddConfig(cfg);
  }

  LOG_INFO << "map configs loaded: count=" << ordered_.size();
  for (auto id : ordered_) {
    auto* c = Find(id);
    LOG_INFO << "  map cfg_id=" << c->cfg_id_
             << " name=" << c->name_
             << " res_id=" << c->res_id_
             << " born=(" << c->born_pos_.GetX() << ","
             << c->born_pos_.GetY() << ","
             << c->born_pos_.GetZ() << ")";
  }
}
