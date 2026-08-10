#pragma once

#include <cstdint>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include "client_3d.pb.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "handlers/handler_base.h"
#include "zrpc/net/tcp_connection.h"

// 3D 空中位移：cli_3d_jump_req（NORMAL/DOUBLE/FALL/SLIDE + START/STEER/LAND）
// 与 cli_3d_move_req 严格分离：空中会话中拒绝 move。
class JumpHandler : public IHandler {
 public:
  void Handle(const ::zrpc::TcpConnectionPtr& conn,
              const EntityPtr& entity,
              uint32_t msg_id,
              const std::shared_ptr<::google::protobuf::Message>& req) override;

  // 离图时清理冷却 + 重置空中状态
  void ClearCooldown(const EntityPtr& entity);
  void ClearCooldown(uint64_t entity_id);

 private:
  static int64_t NowMs();

  void FillJumpRes(::cli_3d_jump_res* res, const EntityPtr& entity,
                   int32_t err_code);
  void SendJumpRes(const ::zrpc::TcpConnectionPtr& conn,
                   const EntityPtr& entity, int32_t err_code);

  bool ValidateStart(const EntityPtr& entity, const TransformComponent* tfm,
                     ::jump_type type, uint32_t jump_id, int32_t* err_out);
  bool ValidateSteerOrLand(const TransformComponent* tfm, uint32_t jump_id,
                           int32_t* err_out);

  void ApplyAirState(TransformComponent* tfm, const ::entity_jump_data& j,
                     ::jump_op op, ::jump_type type);

  std::unordered_map<uint64_t, int64_t> last_jump_ms_;
};
