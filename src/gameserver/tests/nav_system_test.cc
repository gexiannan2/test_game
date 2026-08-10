#include "test_harness.h"

#include <filesystem>

#include "navigation/nav_system.h"

using game::navigation::FollowPath;
using game::navigation::FollowState;
using game::navigation::HeightMapSystem;
using game::navigation::NavPath;
using game::navigation::NavPosition;
using game::navigation::NavStatus;
using game::navigation::NavSystem;
using game::navigation::NavigationBudget;
using game::navigation::SlicedPathQuery;
using game::navigation::SlicedPathState;

GAME_TEST_SUITE(NavSystemTest);

namespace {

std::filesystem::path ProductionNavmeshPath() {
  const std::filesystem::path candidates[] = {
      "deps/map_res/1001.navmesh",
      "../deps/map_res/1001.navmesh",
      "../../deps/map_res/1001.navmesh",
  };
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error) && !error) {
      return candidate;
    }
    error.clear();
  }
  return candidates[0];
}

}  // namespace

GAME_TEST(NavSystemTest, MissingMapReturnsExplicitErrors) {
  NavSystem navigation;
  NavPath path;

  EXPECT_FALSE(navigation.is_loaded(101));
  EXPECT_TRUE(navigation.find_straight_path(
                  101, NavPosition{}, NavPosition{1.0f, 0.0f, 1.0f}, path) ==
              NavStatus::navmesh_not_loaded);
  EXPECT_TRUE(path.empty());

  const auto reachable = navigation.validate_reachable(
      101, NavPosition{}, NavPosition{1.0f, 0.0f, 1.0f});
  EXPECT_TRUE(reachable.status == NavStatus::navmesh_not_loaded);
  EXPECT_FALSE(reachable.reachable);

  const auto position = navigation.check_pos(101, NavPosition{});
  EXPECT_TRUE(position.status == NavStatus::navmesh_not_loaded);
  EXPECT_FALSE(position.valid);

  const auto direct = navigation.validate_direct_move(
      101, NavPosition{}, NavPosition{1.0f, 0.0f, 1.0f});
  EXPECT_TRUE(direct.status == NavStatus::navmesh_not_loaded);
  EXPECT_FALSE(direct.reachable);
}

GAME_TEST(NavSystemTest, ProductionAssetCoversDefaultSpawn) {
  NavSystem navigation;
  const auto path = ProductionNavmeshPath();
  EXPECT_TRUE(std::filesystem::is_regular_file(path));
  EXPECT_TRUE(navigation.load_navmesh(1001, path) == NavStatus::success);

  const NavPosition spawn{333.0f, 18.0f, 415.45f};
  const auto checked = navigation.check_pos(1001, spawn);
  EXPECT_TRUE(checked.status == NavStatus::success);
  EXPECT_TRUE(checked.valid);
  EXPECT_TRUE(checked.horizontal_distance <= 0.5f);

  const auto direct = navigation.validate_direct_move(1001, spawn, spawn);
  EXPECT_TRUE(direct.status == NavStatus::success);
  EXPECT_TRUE(direct.reachable);
}

GAME_TEST(NavSystemTest, CheckPosRejectsInvalidTolerance) {
  NavSystem navigation;
  const auto checked = navigation.check_pos(
      1001, NavPosition{333.0f, 18.0f, 415.45f}, -0.1f);
  EXPECT_TRUE(checked.status == NavStatus::invalid_argument);
  EXPECT_FALSE(checked.valid);
}

GAME_TEST(NavSystemTest, DirectMoveRejectsWallWithoutPathFallback) {
  NavSystem navigation;
  EXPECT_TRUE(navigation.load_navmesh(1001, ProductionNavmeshPath()) ==
              NavStatus::success);

  // 两点均来自 1001.navmesh 的可行走面，直线会在起点附近撞到边界。
  const NavPosition start{347.550f, 4.358f, 244.017f};
  const NavPosition end{343.717f, 30.208f, 607.683f};
  EXPECT_TRUE(navigation.check_pos(1001, start, 0.1f).valid);
  EXPECT_TRUE(navigation.check_pos(1001, end, 0.1f).valid);

  const auto direct = navigation.validate_direct_move(1001, start, end, 0.1f);
  EXPECT_TRUE(direct.status == NavStatus::path_not_found);
  EXPECT_FALSE(direct.reachable);
}

GAME_TEST(NavSystemTest, SlicedQueryFailsWithoutLoadedMap) {
  NavSystem navigation;
  SlicedPathQuery query;

  EXPECT_TRUE(navigation.begin_sliced_path(
                  101, NavPosition{}, NavPosition{5.0f, 0.0f, 5.0f}, query) ==
              NavStatus::navmesh_not_loaded);
  EXPECT_TRUE(query.state == SlicedPathState::failed);
  EXPECT_EQ(query.id, 0u);
}

GAME_TEST(NavSystemTest, FollowFailureIsStatefulAndNonFatal) {
  NavSystem navigation;
  FollowPath follow;
  NavigationBudget budget;

  navigation.start_follow(follow, NavPosition{5.0f, 0.0f, 5.0f});
  const auto update = navigation.update_follow(
      101,
      follow,
      NavPosition{},
      NavPosition{5.0f, 0.0f, 5.0f},
      1.0f / 30.0f,
      budget);

  EXPECT_TRUE(update.state == FollowState::failed);
  EXPECT_TRUE(update.status == NavStatus::navmesh_not_loaded);
  EXPECT_FALSE(update.has_steering_target);
}

GAME_TEST(NavSystemTest, NavigationBudgetLimitsTickWork) {
  NavigationBudget budget;
  budget.max_full_path_queries = 2;
  budget.max_sliced_iterations = 10;

  EXPECT_TRUE(budget.try_consume_full_path());
  EXPECT_TRUE(budget.try_consume_full_path());
  EXPECT_FALSE(budget.try_consume_full_path());
  EXPECT_EQ(budget.consume_sliced_iterations(6), 6);
  EXPECT_EQ(budget.consume_sliced_iterations(6), 4);
  EXPECT_EQ(budget.consume_sliced_iterations(1), 0);

  budget.reset();
  EXPECT_EQ(budget.used_full_path_queries, 0u);
  EXPECT_EQ(budget.used_sliced_iterations, 0);
}

GAME_TEST(NavSystemTest, HeightMapMissingMapIsExplicit) {
  HeightMapSystem height_maps;
  float height = 0.0f;
  std::vector<float> layers;

  EXPECT_FALSE(height_maps.is_loaded(101));
  EXPECT_TRUE(height_maps.query(101, 0.0f, 0.0f, height) ==
              NavStatus::heightmap_not_loaded);
  EXPECT_TRUE(height_maps.query_all_layers(101, 0.0f, 0.0f, layers) ==
              NavStatus::heightmap_not_loaded);
  EXPECT_TRUE(layers.empty());
}
