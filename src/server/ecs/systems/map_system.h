#pragma once

// 无界稀疏地图：按 kGridSize 切逻辑格，实体单格占位；视野由 AoiSystem 管理。

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

    void SetEnterMapCallback(EnterMapCallback cb);
    void SetLeaveMapCallback(LeaveMapCallback cb);
    void SetMoveCallback(MoveCallback cb);
    void SetCrossGridCallback(CrossGridCallback cb);

    void NotifyEnterMap(const EntityPtr& entity);
    void NotifyLeaveMap(const EntityPtr& entity);
    void NotifyMove(const EntityPtr& entity, const Vector3D& old_pos,
                   const Vector3D& new_pos);
    void NotifyCrossGrid(const EntityPtr& entity, int32_t old_gx, int32_t old_gy,
                        int32_t old_gz, int32_t new_gx, int32_t new_gy,
                        int32_t new_gz);

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
    EnterMapCallback enter_map_cb_;
    LeaveMapCallback leave_map_cb_;
    MoveCallback move_cb_;
    CrossGridCallback cross_grid_cb_;
};
