// Vector3D / Map / AOI 坐标换算（负坐标格索引、格中心、AOI 格比例）。
#include "test_harness.h"

#include "common/vector3d.h"
#include "common/aoi_def.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"
#include "test_map_invariants.h"

GAME_TEST_SUITE(MathTest);

GAME_TEST(MathTest, GridIndexNegativeCoordinates) {
  Vector3D pos(-10.f, -5.f, -1.f);
  EXPECT_EQ(pos.GridX(), -3);
  EXPECT_EQ(pos.GridY(), -2);
  EXPECT_EQ(pos.GridZ(), -1);

  Vector3D edge(-8.f, 0.f, 0.f);
  EXPECT_EQ(edge.GridX(), -2);
  EXPECT_EQ(edge.GridY(), 0);
}

GAME_TEST(MathTest, MapIndexToCenterPosNegativeGrid) {
  MapSystem map(SceneRegionType::kMap);
  map.Init();
  Vector3D center;
  EXPECT_TRUE(map.MapIndexToCenterPos(-1, -1, 0, center));
  EXPECT_NEAR(center.GetX(), -2.f, 0.001f);
  EXPECT_NEAR(center.GetY(), -2.f, 0.001f);
}

GAME_TEST(MathTest, AoiCellWorldSizeVsMapGrid) {
  EXPECT_EQ(kGridSize, 4u);
  EXPECT_EQ(kAoiCellWorldSize, 10u);
  // 地图格 4m，视野格 10m：同一 AOI 格覆盖多个地图格
  int32_t gx0 = 0;
  int32_t gy0 = 0;
  int32_t gz0 = 0;
  int32_t gx1 = 0;
  int32_t gy1 = 0;
  int32_t gz1 = 0;
  AoiCellToMapGridBox(0, 0, 0, gx0, gy0, gz0, gx1, gy1, gz1);
  EXPECT_EQ(gx0, 0);
  EXPECT_EQ(gx1, 3);  // world [0,10) → map grids 0,1,2

  Vector3D in_same_aoi(7.f, 7.f, 7.f);
  Vector3D next_aoi(11.f, 7.f, 7.f);
  EXPECT_EQ(in_same_aoi.AoiCellX(), 0);
  EXPECT_EQ(next_aoi.AoiCellX(), 1);
  EXPECT_EQ(in_same_aoi.GridX(), 1);
  EXPECT_EQ(next_aoi.GridX(), 2);
}

GAME_TEST(MathTest, NegativeWorldPosFootprintOnMap) {
  auto world = test::MakeWorld();
  EntityPtr e = world->Spawn(
      EntityType::kMarch, EntitySpawn::At(Vector3D(-10.f, -5.f, -1.f)));
  world->EnterMap(e);
  EXPECT_TRUE(test::FootprintContainsEntity(world->Map(), e->GetPosition(),
                                            e->GetId()));
  world->LeaveMap(e);
}
