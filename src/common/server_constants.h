#pragma once

#include <cstdint>
#include <cstdlib>

namespace server {

inline constexpr uint32_t kDefaultMapInstanceId = 1;
inline constexpr uint32_t kDefaultLineId = 1;

inline constexpr uint32_t kKickoffServer = 1;
inline constexpr uint32_t kKickoffReplaceAccount = 2;
inline constexpr uint32_t kKickoffReplaceRole = 3;

inline constexpr int32_t kLoginErrOk = 0;
inline constexpr int32_t kLoginErrEmptyUid = 1;
inline constexpr int32_t kLoginErrNeedHandshake = 2;
inline constexpr int32_t kLoginErrBadRequest = 3;

inline constexpr int32_t kEnterGameErrOk = 0;
inline constexpr int32_t kEnterGameErrBadState = 1;
inline constexpr int32_t kEnterGameErrNoMap = 2;
inline constexpr int32_t kEnterGameErrAlreadyInMap = 3;
inline constexpr int32_t kEnterGameErrConnLost = 4;
inline constexpr int32_t kEnterGameErrNoRole = 5;

inline constexpr int32_t kRoleErrOk = 0;
inline constexpr int32_t kRoleErrFailed = 1;

inline constexpr int32_t kRoleListOpQuery = 1;
inline constexpr int32_t kRoleListOpCreate = 2;
inline constexpr int32_t kRoleListOpDelete = 3;

inline constexpr int32_t kMoveSuccess = 1;
inline constexpr int32_t kMoveFailed = 0;

// 跳跃 / 空中位移
inline constexpr int32_t kJumpErrOk              = 0;  // 成功
inline constexpr int32_t kJumpErrBadState        = 1;  // 未进图/状态异常
inline constexpr int32_t kJumpErrCooldown        = 2;  // 冷却中
inline constexpr int32_t kJumpErrBadPos          = 3;  // 位置非法
inline constexpr int32_t kJumpErrBadParam        = 4;  // 参数非法
inline constexpr int32_t kJumpErrBadAirState     = 5;  // 空中/地面状态不匹配
inline constexpr int32_t kJumpErrAirJumpExhausted = 6; // 二段跳次数耗尽
inline constexpr int32_t kJumpErrJumpIdMismatch  = 7;  // jump_id 与空中会话不符

inline constexpr int64_t kJumpCooldownMs = 300;
inline constexpr float   kJumpMaxDist    = 10.0f;
inline constexpr uint32_t kMaxAirJumps   = 1;  // 除首段外最多再跳 1 次（二段跳）

// 移动：空中禁止用 move（须走 jump）
inline constexpr int32_t kMoveErrOk            = 0;
inline constexpr int32_t kMoveErrFailed        = 1;
inline constexpr int32_t kMoveErrAirborne      = 2;  // 空中会话中禁止 move
inline constexpr int32_t kMoveErrUseJumpProto  = 3;  // status 要求走 jump 协议
inline constexpr int32_t kMoveErrNavUnavailable = 4; // 导航资源未就绪
inline constexpr int32_t kMoveErrNavRejected   = 11; // 目标点或直线移动不可通行

}  // namespace server
