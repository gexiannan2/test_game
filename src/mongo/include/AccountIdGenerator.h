#pragma once

#include <cstdint>

namespace mongo
{
    // account_info._id 复合 ID 编码：
    //   _id = account_id * INDEX_MOD_NUM + area_id
    // 反解：
    //   account_id = _id / INDEX_MOD_NUM
    //   area_id    = _id % INDEX_MOD_NUM
    //
    // 约束：area_id 必须 ∈ [0, INDEX_MOD_NUM)。
    //       account_id 应满足 account_id * INDEX_MOD_NUM 不溢出 int64。
    //
    // account_info._id 不再使用业务字符串拼接，改为数字型复合 ID，
    // 便于通过 _id 直接反解原始 account_id 与 area_id，也减少索引体积。
    constexpr std::int64_t INDEX_MOD_NUM = 1000000;

    // 计算 account_info 文档 _id：account_id * INDEX_MOD_NUM + area_id。
    // 调用方必须先确保 area_id ∈ [0, INDEX_MOD_NUM)，否则结果不可逆。
    inline std::int64_t MakeAccountInfoId(std::int64_t accountId, std::int64_t areaId) noexcept
    {
        return accountId * INDEX_MOD_NUM + areaId;
    }

    // 反解 _id 取 account_id。
    inline std::int64_t GetAccountIdFromId(std::int64_t id) noexcept
    {
        return id / INDEX_MOD_NUM;
    }

    // 反解 _id 取 area_id。
    inline std::int64_t GetAreaIdFromId(std::int64_t id) noexcept
    {
        return id % INDEX_MOD_NUM;
    }

    // 校验 area_id 是否在合法编码区间，避免 _id 不可逆或越界。
    inline bool IsValidAreaId(std::int64_t areaId) noexcept
    {
        return areaId >= 0 && areaId < INDEX_MOD_NUM;
    }

    // 生成 account_id：64 位雪崩型 ID，移植自 e996::generate_id。
    //
    // 算法：account_id = (sid << 32) | master_id
    //   sid        = 进程内自增子 ID（uint32，溢出回绕时刷新 master_id）
    //   master_id  = [MAC 哈希 16bit][毫秒时间戳 8bit][随机 8bit]，再做 MurmurHash3 finalizer 扰乱
    //
    // 特性：
    //   - 单进程内并发安全（std::mutex 串行化）
    //   - 单进程单 master_id 周期内 sid 严格单调自增，故连续两次调用结果不同
    //   - 跨进程由 MAC 哈希区分；同主机重启理论上有极小概率 master_id 碰撞，
    //     业务侧必须靠 account_id 字段唯一索引兜底（重复时捕获 E11000 重试）
    //   - 不能从 ID 反解生成时间（仅保留毫秒低 8 位）
    std::uint64_t GenerateAccountId();

} // namespace mongo
