#pragma once

#include <cstdint>

namespace server {

inline constexpr std::int32_t kLoginErrOk = 0;
inline constexpr std::int32_t kLoginErrEmptyUid = 2;

inline constexpr std::int32_t kRoleErrOk = 0;
inline constexpr std::int32_t kRoleErrFailed = 1;
inline constexpr std::int32_t kRoleListOpQuery = 1;
inline constexpr std::int32_t kRoleListOpCreate = 2;
inline constexpr std::int32_t kRoleListOpDelete = 3;

inline constexpr std::int32_t kEnterGameErrOk = 0;
inline constexpr std::int32_t kEnterGameErrAlreadyInMap = 7;
inline constexpr std::int32_t kEnterGameErrBadState = 7;
inline constexpr std::int32_t kEnterGameErrNoRole = 4;
inline constexpr std::int32_t kEnterGameErrConnLost = 1;
inline constexpr std::int32_t kEnterGameErrNoMap = 4;

inline constexpr std::uint32_t kKickoffReplaceAccount = 2;
inline constexpr std::uint32_t kKickoffReplaceRole = 3;
inline constexpr std::uint32_t kDefaultLineId = 1;
inline constexpr std::uint64_t kDefaultMapInstanceId = 1;

}  // namespace server
