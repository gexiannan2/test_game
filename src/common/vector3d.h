// vector3d.h — JPH::Vec3 适配层（大世界坐标 + 逻辑格 / AOI 视野格换算）
// 从 aoi_def.h 抽出，供 map/aoi/move/entity 等模块共用。
#pragma once

#include <cmath>
#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>

// 地图逻辑格边长（世界单位）。实体占地、MapGrid 索引 = floor(world / kGridSize)。
constexpr uint32_t kGridSize = 4;

// AOI 视野格边长（世界单位）。比地图格大，避免玩家每走一格地图格就触发视野同步。
// 观察者邻域为 ±kNeighborhoodRadius 个 AOI 格（见 aoi_def.h），默认半径 0 → 10×10×10 立方。
constexpr uint32_t kAoiCellWorldSize = 10;

// 世界坐标 → 地图逻辑格索引（负坐标用 floor）
inline int32_t WorldToMapGridIndex(float world, uint32_t grid_size = kGridSize) {
    return static_cast<int32_t>(
        std::floor(world / static_cast<float>(grid_size)));
}

// 世界坐标 → AOI 视野格索引
inline int32_t WorldToAoiCellIndex(float world,
                                   uint32_t aoi_cell_size = kAoiCellWorldSize) {
    return static_cast<int32_t>(
        std::floor(world / static_cast<float>(aoi_cell_size)));
}

// 地图逻辑格索引 → 格中心世界坐标（与 MapSystem::MapIndexToCenterPos 一致）
inline float MapGridIndexToCenterWorld(int32_t grid_index,
                                       uint32_t grid_size = kGridSize) {
    const float half = static_cast<float>(grid_size) * 0.5f;
    return half + static_cast<float>(grid_index) * static_cast<float>(grid_size);
}

// AOI 视野格索引 → 该格在 X 轴上的半开区间 [min, max)（世界坐标）
inline void AoiCellWorldRangeX(int32_t aoi_cell_x, float& min_world,
                               float& max_world,
                               uint32_t aoi_cell_size = kAoiCellWorldSize) {
    const float cs = static_cast<float>(aoi_cell_size);
    min_world = static_cast<float>(aoi_cell_x) * cs;
    max_world = min_world + cs;
}

// AOI 视野格 → 覆盖的地图逻辑格半开区间 [gx0, gx1)（供 CollectEntitiesInGridBox）
inline void AoiCellToMapGridBox(int32_t aoi_cx, int32_t aoi_cy, int32_t aoi_cz,
                                  int32_t& gx0, int32_t& gy0, int32_t& gz0,
                                  int32_t& gx1, int32_t& gy1, int32_t& gz1,
                                  uint32_t aoi_cell_size = kAoiCellWorldSize,
                                  uint32_t grid_size = kGridSize) {
    float wx0 = 0.f;
    float wx1 = 0.f;
    float wy0 = 0.f;
    float wy1 = 0.f;
    float wz0 = 0.f;
    float wz1 = 0.f;
    AoiCellWorldRangeX(aoi_cx, wx0, wx1, aoi_cell_size);
    AoiCellWorldRangeX(aoi_cy, wy0, wy1, aoi_cell_size);
    AoiCellWorldRangeX(aoi_cz, wz0, wz1, aoi_cell_size);
    const float gs = static_cast<float>(grid_size);
    gx0 = static_cast<int32_t>(std::floor(wx0 / gs));
    gy0 = static_cast<int32_t>(std::floor(wy0 / gs));
    gz0 = static_cast<int32_t>(std::floor(wz0 / gs));
    gx1 = static_cast<int32_t>(std::ceil(wx1 / gs));
    gy1 = static_cast<int32_t>(std::ceil(wy1 / gs));
    gz1 = static_cast<int32_t>(std::ceil(wz1 / gs));
}

// Vector3D — 继承 JPH::Vec3，补充 GridX/GridY/GridZ 与 Set。
class Vector3D : public JPH::Vec3 {
 public:
    Vector3D() : JPH::Vec3(JPH::Vec3::sZero()) {}
    Vector3D(float x, float y, float z) : JPH::Vec3(x, y, z) {}
    Vector3D(const JPH::Vec3& v) : JPH::Vec3(v) {}
    Vector3D& operator=(const JPH::Vec3& v) {
        JPH::Vec3::operator=(v);
        return *this;
    }

    int32_t GridX() const { return WorldToMapGridIndex(GetX()); }
    int32_t GridY() const { return WorldToMapGridIndex(GetY()); }
    int32_t GridZ() const { return WorldToMapGridIndex(GetZ()); }

    int32_t AoiCellX() const { return WorldToAoiCellIndex(GetX()); }
    int32_t AoiCellY() const { return WorldToAoiCellIndex(GetY()); }
    int32_t AoiCellZ() const { return WorldToAoiCellIndex(GetZ()); }

    void Set(float x, float y, float z) {
        SetX(x);
        SetY(y);
        SetZ(z);
    }
};
