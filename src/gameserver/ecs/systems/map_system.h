#pragma once

// 无界稀疏地图：按 kGridSize 切逻辑格，实体单格占位；视野由 AoiSystem 管理。
//
// 进/离/更新地图的生命周期事件由 WorldSystem 统一通过 EventBus 广播
// （EvtEnterMap / EvtLeaveMap / EvtMoveMap），MapSystem 只负责空间索引，
// 不再持有业务回调。

#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

#include "common/aoi_def.h"
#include "ecs/systems/map_grid.h"

enum class PathCalcResult : int {
    kNone = 0,
    kPopTarget,
    kMoveToTarget,
};

class MapSystem {
 public:
    explicit MapSystem(SceneRegionType system_type);
    ~MapSystem();
    MapSystem(const MapSystem&) = delete;
    MapSystem& operator=(const MapSystem&) = delete;

    void Init();
    MapGridStorage GridStorage() const { return MapGridStorage::kHash; }
    size_t AllocatedGridCount() const;

    void OnEntityIntoMap(EntityPtr entity);
    void OnEntityLeaveMap(EntityPtr entity);
    void OnEntityChangePos(EntityPtr entity, const Vector3D& old_pos,
                          const Vector3D& new_pos);

    bool IsInMap(const Vector3D& /*pos*/) const { return true; }

    bool MapIndexToCenterPos(int32_t x, int32_t y, int32_t z,
                             Vector3D& pos) const;

    MapGrid* GetGrid(int32_t x, int32_t y, int32_t z);
    MapGrid* GetGrid(const Vector3D& pos);
    const MapGrid* GetGrid(int32_t x, int32_t y, int32_t z) const;
    void CollectEntitiesInGridRect(
        int32_t gx_begin, int32_t gy_begin, int32_t gx_end_excl,
        int32_t gy_end_excl,
        const std::function<void(const EntityPtr&)>& fn) const;
    void CollectEntitiesInGridBox(
        int32_t gx_begin, int32_t gy_begin, int32_t gz_begin,
        int32_t gx_end_excl, int32_t gy_end_excl, int32_t gz_end_excl,
        const std::function<void(const EntityPtr&)>& fn) const;

    bool LineInterGrid(const Vector3D& pos_0, const Vector3D& pos_1,
                       std::list<GridKey>& grid_keys);

    bool IsRelationEntity(const Vector3D& start_pos, const Vector3D& target_pos,
                          const Vector3D& center_pos, float radius,
                          Vector3D& cvt) const;
    PathCalcResult CalcMoveSpherePath(const Vector3D& start_pos,
                                      const Vector3D& target_pos,
                                      const Vector3D& center_pos, float radius,
                                      std::list<Vector3D>& paths);

 private:
    void MakeArcPath(const Vector3D& pos_1, const Vector3D& pos_2,
                     const Vector3D& center_pos, std::list<Vector3D>& paths);
    const MapGrid* FindGrid(const GridKey& key) const;
    MapGrid* FindGrid(const GridKey& key);
    MapGrid* EnsureGrid(int32_t x, int32_t y, int32_t z);
    void ReleaseHashGridIfEmpty(const GridKey& key);

    SceneRegionType system_type_;
    std::unordered_map<GridKey, MapGrid, GridKeyHash> hash_grids_;
};
