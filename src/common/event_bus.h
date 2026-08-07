#pragma once

#include <any>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "ecs/entity/entity.h"

// ============================================================
// EventBus — 轻量级事件驱动广播系统
//
// 用法:
//   // 1. 定义事件（struct，携带需要的数据）
//   struct EvtLeaveMap { EntityPtr entity; };
//
//   // 2. 监听者注册（任意模块初始化时）
//   bus.Subscribe<EvtLeaveMap>([](const EvtLeaveMap& ev) {
//       clear_cooldown(ev.entity->GetId());
//   });
//
//   // 3. 发布者触发（业务代码中）
//   bus.Publish(EvtLeaveMap{entity});
//
// 所有回调在同一线程（主线程）同步执行，无需加锁。
// ============================================================

class EventBus {
 public:
  static EventBus& Instance() {
    static EventBus inst;
    return inst;
  }

  // 注册事件监听，返回 listener_id（用于反注册，0 表示失败）
  template <typename EventT>
  uint32_t Subscribe(std::function<void(const EventT&)> cb) {
    if (!cb) return 0;
    uint32_t id = next_id_++;
    handlers_[std::type_index(typeid(EventT))].push_back(
        {id, [cb = std::move(cb)](const std::any& ev) {
             cb(std::any_cast<const EventT&>(ev));
         }});
    return id;
  }

  // 反注册
  template <typename EventT>
  void Unsubscribe(uint32_t id) {
    auto it = handlers_.find(std::type_index(typeid(EventT)));
    if (it == handlers_.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [id](const Handler& h) { return h.id == id; }),
              vec.end());
  }

  // 发布事件，同步调用所有监听者
  template <typename EventT>
  void Publish(const EventT& event) {
    auto it = handlers_.find(std::type_index(typeid(EventT)));
    if (it == handlers_.end()) return;
    // 拷贝一份，防止回调中修改 vector（Unsubscribe）
    auto snapshot = it->second;
    for (auto& h : snapshot) {
      h.fn(event);
    }
  }

  // 清空所有监听（服务器关闭时）
  void Clear() { handlers_.clear(); }

 private:
  struct Handler {
    uint32_t id;
    std::function<void(const std::any&)> fn;
  };

  std::unordered_map<std::type_index, std::vector<Handler>> handlers_;
  uint32_t next_id_ = 1;
};

// ============================================================
// 标准事件类型定义
// ============================================================

// 进图：WorldSystem::EnterMap 发布
struct EvtEnterMap {
  EntityPtr entity;
};

// 离图：WorldSystem::LeaveMap 发布
struct EvtLeaveMap {
  EntityPtr entity;
};

// 断线：GameServer::OnDisconnect 发布
struct EvtDisconnect {
  EntityPtr entity;
};

// 跨格移动：AOI 跨格时发布
struct EvtCrossGrid {
  EntityPtr entity;
  Vector3D old_pos;
  Vector3D new_pos;
};

// 更新地图位置：WorldSystem::MoveEntity 跨格时发布。
// 进/离图分别用 EvtEnterMap / EvtLeaveMap，三者共同构成"地图生命周期 + 位移"事件族。
// 业务模块（jump 冷却、统计、成就等）通过 Subscribe 监听，不再经由 MapSystem 回调。
struct EvtMoveMap {
  EntityPtr entity;
  Vector3D old_pos;
  Vector3D new_pos;
};
