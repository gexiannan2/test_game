#pragma once

// 服务端路径移动（NPC/行军）；玩家移动走 handler 直写坐标，不经本组件。
// 状态机：None → PathFind → Moving（可选 MoveArc）→ None

#include <functional>
#include <list>

#include "ecs/component_base/component_base.h"
#include "common/aoi_def.h"  // MoveStopReason, Vector3D, EntityPtr

class WorldSystem;
class Entity;
class MoveSystem;

using MoveStopCallback = std::function<void(bool /*success*/, MoveStopReason)>;

// 一段寻路折线（pass_id 便于调试/打断识别）
struct StatePathData3D {
  uint64_t pass_id = 0;
  std::list<Vector3D> paths;
};

class MoveComponent : public IComponent {
 public:
  enum MoveStatus {
    kMoveStatusNone = 0,
    kMoveStatusMoving,
    kMoveStatusPathFind,
    kMoveStatusMoveArc,
  };

  ComponentType Type() const override { return ComponentType::kMove; }

  void Clear();
  bool IsMoving() const { return move_status_ != kMoveStatusNone; }

  // 请求移动到目标；接受后进入 PathFind，当前实现为直线终点（可扩展真实寻路）
  bool MoveTo(const Vector3D& pos, MoveStopCallback cb);

  void SetSpeed(float speed) { move_speed_ = speed; }
  float GetCurSpeed() const { return move_speed_; }

  const Vector3D& GetDestination() const { return target_position_; }

  // 主动停止（打断/取消）；触发完成回调并 Clear
  void OnStopMove(bool is_done, MoveStopReason reason);

  // 由 MoveSystem 每帧调用：推进位移并同步到 WorldSystem::MoveEntity
  void TickFrame(WorldSystem* world, Entity* entity, float dt_sec = 1.f / 60.f);

 private:
  friend class MoveSystem;

  void CheckPreObstacle(WorldSystem* world, Entity* entity);
  void HandleMove(WorldSystem* world, Entity* entity, float dt_sec);
  void StartMove(Entity* entity);
  bool UpdateMovePos(WorldSystem* world, Entity* entity, float dt_sec);
  bool PopMoveTarget();
  bool PopCurMoveTarget();
  void InitPathFindData(const std::list<Vector3D>& find_path_list);
  void OnPathFindResult(Entity* entity, const std::list<Vector3D>& find_path_list);
  void DoFindPath(Entity* entity, const Vector3D& start, const Vector3D& end);
  bool IsEmptyMoveTarget() const;
  Vector3D* GetMoveTarget();
  Vector3D* GetPathMoveTarget();

  MoveStatus move_status_ = kMoveStatusNone;
  MoveStopCallback move_call_back_;
  std::list<Vector3D> move_arc_paths_;
  Vector3D pre_check_obstacle_pos_{-1.f, -1.f, -1.f};
  bool pre_check_obstacle_valid_ = false;
  float move_speed_ = 0.f;
  Vector3D target_position_;
  std::list<StatePathData3D> cur_region_paths_;
  Vector3D facing_dir_{1.f, 0.f, 0.f};  // 本帧行进方向（未归一化亦可）
};

extern uint64_t g_pass_id_3d;

DECLARE_COMPONENT(MoveComponent, ComponentType::kMove)
