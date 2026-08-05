#pragma once

// AOI/Map 共用定义（移植自 move3，适配本工程 EntityPtr / Vector3D）。

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <list>

#include "common/vector3d.h"

class Entity;
class WorldSystem;
class MapSystem;
class MoveComponent;
class AoiSector;

using EntityPtr = std::shared_ptr<Entity>;

enum class MoveStopReason : int {
    kSuccess = 0,
    kPathFindError,
    kZeroSpeed,
    kPassClosed,
    kStopCommand,
    kMarchAi,
    kUnknown = 100,
};

enum class SceneComponentType : int {
    kNone = 0,
    kOwner,
    kEffect,
    kStatus,
    kMove,
    kArmy,
};

enum class SceneRegionType : int {
    kNone = 0,
    kMap,
    kAoi,
    kPlayer,
    kPathFind,
};

enum class MapCellStatus : uint32_t {
    kNone = 0,
    kObstacle = 1u << 0,
    kNoRefresh = 1u << 1,
    kNoFly = 1u << 2,
    kWalkable = 1u << 3,
};

enum class MapGridStorage : uint8_t {
    kArray = 0,
    kHash = 1,
};

enum class EntityType : uint32_t {
    kTown = 1,
    kMarch = 2,
    kPlayer = 3,
};

constexpr uint32_t kInvalidGridIndex = 0xffffffffu;

// 观察者邻域半径（±1 个 AOI 格）。
// 必须 ≥1：避免格子边缘盲区——相距 <1m 的两个实体若落在相邻格，
// 半径=0 时互不可见。半径=1 保证实体永远看到周围所有相邻格（3×3×3=27 格）。
constexpr int32_t kAoiRadius = 1;

inline MapGridStorage DefaultMapGridStorage() {
    return MapGridStorage::kHash;
}

struct GridKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const GridKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
    bool operator<(const GridKey& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

struct GridKeyHash {
    size_t operator()(const GridKey& k) const {
        return (static_cast<size_t>(static_cast<uint32_t>(k.x)) * 73856093u) ^
               (static_cast<size_t>(static_cast<uint32_t>(k.y)) * 19349663u) ^
               (static_cast<size_t>(static_cast<uint32_t>(k.z)) * 83492791u);
    }
};

inline constexpr uint32_t kAoiFarCellWorldSize = 500;
inline constexpr uint32_t kDefaultCellSizes[] = { kAoiCellWorldSize };
inline constexpr size_t kDetailLevelCount = 1;
inline constexpr int32_t kNeighborhoodRadius = kAoiRadius;

enum class AoiEventKind {
    kEnter = 0,
    kLeave,
    kUpdate,
};

struct AoiEvent {
    AoiEventKind kind;
    uint64_t watcher_id;
    std::vector<uint64_t> entity_ids;
};

using ViewNotifyFn = std::function<void(const AoiEvent&)>;

// per-subject 广播事件：一个 subject 通知一批 watcher。
// watcher_ids 已排除 subject 自身（EntityMonitor 收集时过滤），
// 桥接层据此“序列化一次遍历发送”，把 M×N 次序列化降为 per-subject 1 次。
struct AoiBroadcastEvent {
    AoiEventKind kind;
    uint64_t subject_id = 0;
    std::vector<uint64_t> watcher_ids;
};

using AoiBroadcastNotifyFn = std::function<void(const AoiBroadcastEvent&)>;

using EnterMapCallback =
    std::function<void(const EntityPtr&)>;
using LeaveMapCallback =
    std::function<void(const EntityPtr&)>;
using MoveCallback =
    std::function<void(const EntityPtr&, const Vector3D&, const Vector3D&)>;
using CrossGridCallback =
    std::function<void(const EntityPtr&, uint32_t, uint32_t, uint32_t,
                       uint32_t, uint32_t, uint32_t)>;

using EntityEnterCallback =
    std::function<void(uint64_t, const std::vector<uint64_t>&)>;
using EntityLeaveCallback =
    std::function<void(uint64_t, const std::vector<uint64_t>&)>;
using EntityUpdateCallback =
    std::function<void(uint64_t, uint64_t)>;

using MoveCompleteCallback =
    std::function<void(const EntityPtr&, bool, MoveStopReason)>;

class AoiSector;
