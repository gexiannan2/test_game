// 地图静态配置加载（demo 硬编码，正式可换读表）。

#include "ecs/systems/map_config_system.h"

#include "zrpc/base/logger.h"

void MapConfigSystem::LoadDefaults() {
  {
    MapConfig cfg;
    cfg.cfg_id_        = 1001;
    cfg.name_          = "新手村";
    cfg.res_id_        = "1001";
    cfg.dist_flag_     = 0;
    cfg.map_type_      = 0;
    cfg.born_pos_      = JPH::Vec3(333.0f, 18.0f, 415.45f);
    cfg.born_rot_      = JPH::Quat::sIdentity();
    cfg.born_move_rot_ = JPH::Quat::sIdentity();
    cfg.born_range_    = 3.0f;
    AddConfig(cfg);
  }

  {
    MapConfig cfg;
    cfg.cfg_id_        = 1201;
    cfg.name_          = "野外森林";
    cfg.res_id_        = "1201";
    cfg.dist_flag_     = 0;
    cfg.map_type_      = 0;
    cfg.born_pos_      = JPH::Vec3(333.80f, 4.36f, 303.43f);
    cfg.born_rot_      = JPH::Quat::sIdentity();
    cfg.born_move_rot_ = JPH::Quat::sIdentity();
    cfg.born_range_    = 3.0f;
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
