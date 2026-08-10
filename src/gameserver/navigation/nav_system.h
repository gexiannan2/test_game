#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace KBEngine
{
class NavMeshHandle;
}

namespace game::navigation
{

using map_id_t = std::uint32_t;
using NavMeshHandle = KBEngine::NavMeshHandle;

struct NavPosition
{
    // Server/world convention: X/Z are horizontal axes and Y is vertical.
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class NavStatus : std::uint8_t
{
    success,
    in_progress,
    partial_path,
    invalid_argument,
    navmesh_not_loaded,
    heightmap_not_loaded,
    layer_not_found,
    nearest_poly_not_found,
    path_not_found,
    query_failed,
    query_cancelled,
    query_not_found,
    out_of_range,
    height_mismatch,
    obstacle_limit_reached,
};

[[nodiscard]] constexpr bool nav_status_succeeded(NavStatus status) noexcept
{
    return status == NavStatus::success;
}

[[nodiscard]] constexpr bool nav_status_has_path(NavStatus status) noexcept
{
    return status == NavStatus::success || status == NavStatus::partial_path;
}

[[nodiscard]] std::string_view to_string(NavStatus status) noexcept;

struct NavQueryOptions
{
    NavPosition nearest_poly_extents{2.0f, 4.0f, 2.0f};
    int max_path_polys = 256;
    int max_straight_path_points = 256;
};

struct NavPath
{
    std::vector<NavPosition> points;
    bool partial = false;

    void clear() noexcept
    {
        points.clear();
        partial = false;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return points.empty();
    }
};

struct ReachabilityResult
{
    NavStatus status = NavStatus::query_failed;
    bool reachable = false;
    NavPosition position{};
};

enum class SlicedPathState : std::uint8_t
{
    idle,
    in_progress,
    ready,
    finalized,
    cancelled,
    failed,
};

struct SlicedPathQuery
{
    std::uint64_t id = 0;
    map_id_t map_id = 0;
    SlicedPathState state = SlicedPathState::idle;
    NavStatus status = NavStatus::success;

    [[nodiscard]] bool active() const noexcept
    {
        return id != 0 &&
            (state == SlicedPathState::in_progress || state == SlicedPathState::ready);
    }
};

enum class FollowState : std::uint8_t
{
    idle,
    path_pending,
    following,
    repath_required,
    arrived,
    failed,
};

struct FollowConfig
{
    float target_move_threshold = 1.0f;
    float waypoint_reach_distance = 0.35f;
    float arrival_distance = 0.45f;
    float path_deviation_threshold = 1.5f;
    float repath_interval_seconds = 0.30f;
    std::uint32_t max_repath_failures = 3;
    bool use_sliced_path = false;
    int sliced_iterations_per_tick = 32;
};

struct FollowPath
{
    FollowState state = FollowState::idle;
    NavStatus last_status = NavStatus::success;
    NavPath path;
    std::size_t waypoint_index = 0;
    std::vector<std::uint64_t> corridor_refs;
    NavPosition target{};
    NavPosition last_repath_target{};
    float seconds_since_repath = 0.0f;
    std::uint32_t consecutive_failures = 0;
    SlicedPathQuery pending_path;

    void reset() noexcept;
};

struct NavigationBudget
{
    std::size_t max_full_path_queries = 8;
    std::size_t used_full_path_queries = 0;
    int max_sliced_iterations = 256;
    int used_sliced_iterations = 0;

    void reset() noexcept;
    [[nodiscard]] bool try_consume_full_path() noexcept;
    [[nodiscard]] int consume_sliced_iterations(int requested) noexcept;
};

struct FollowUpdate
{
    FollowState state = FollowState::idle;
    NavStatus status = NavStatus::success;
    bool has_steering_target = false;
    NavPosition steering_target{};
    bool repath_started = false;
    bool path_replaced = false;
};

struct HeightMapInfo
{
    float origin_x = 0.0f;
    float origin_z = 0.0f;
    int width = 0;
    int depth = 0;
    float cell_size = 0.0f;
    float max_height = 0.0f;
};

struct HeightLayerResult
{
    NavStatus status = NavStatus::query_failed;
    float height = 0.0f;
    std::int8_t layer = -1;
    bool switched = false;
};

struct PlayerMoveValidation
{
    NavStatus status = NavStatus::query_failed;
    NavPosition server_position{};
    std::int8_t height_layer = -1;
    bool layer_switched = false;
};

class HeightMapSystem;

class NavSystem final
{
public:
    // Tick-thread service: all methods on one instance must be called from the
    // same game thread. Sliced queries may span ticks, but must not change thread.
    NavSystem();
    ~NavSystem();

    NavSystem(const NavSystem&) = delete;
    NavSystem& operator=(const NavSystem&) = delete;
    NavSystem(NavSystem&&) noexcept;
    NavSystem& operator=(NavSystem&&) noexcept;

    [[nodiscard]] NavStatus load_navmesh(map_id_t map_id, const std::filesystem::path& file_path);
    [[nodiscard]] NavStatus load_navmesh_directory(
        const std::filesystem::path& directory,
        std::size_t& loaded_count);
    [[nodiscard]] bool unload_navmesh(map_id_t map_id) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool is_loaded(map_id_t map_id) const noexcept;
    [[nodiscard]] NavMeshHandle* get_navmesh(map_id_t map_id) noexcept;
    [[nodiscard]] const NavMeshHandle* get_navmesh(map_id_t map_id) const noexcept;

    [[nodiscard]] NavStatus find_nearest_position(
        map_id_t map_id,
        const NavPosition& position,
        NavPosition& nearest,
        const NavPosition& extents = {2.0f, 4.0f, 2.0f}) const;

    [[nodiscard]] NavStatus find_straight_path(
        map_id_t map_id,
        const NavPosition& from,
        const NavPosition& to,
        NavPath& result,
        const NavQueryOptions& options = {}) const;

    [[nodiscard]] ReachabilityResult validate_reachable(
        map_id_t map_id,
        const NavPosition& from,
        const NavPosition& to,
        const NavQueryOptions& options = {}) const;

    // NavMesh poly height is auxiliary only. Authoritative actor Y comes from HeightMapSystem.
    [[nodiscard]] NavStatus get_poly_height(
        map_id_t map_id,
        const NavPosition& position,
        float& height) const;

    [[nodiscard]] NavStatus begin_sliced_path(
        map_id_t map_id,
        const NavPosition& from,
        const NavPosition& to,
        SlicedPathQuery& query,
        const NavQueryOptions& options = {});

    [[nodiscard]] NavStatus update_sliced_path(
        SlicedPathQuery& query,
        int max_iterations,
        int& completed_iterations);

    [[nodiscard]] NavStatus finalize_sliced_path(
        SlicedPathQuery& query,
        NavPath& result);

    void cancel_sliced_path(SlicedPathQuery& query) noexcept;

    void start_follow(FollowPath& follow, const NavPosition& target) noexcept;
    void stop_follow(FollowPath& follow) noexcept;

    [[nodiscard]] FollowUpdate update_follow(
        map_id_t map_id,
        FollowPath& follow,
        const NavPosition& current_position,
        const NavPosition& target,
        float delta_seconds,
        NavigationBudget& budget,
        const FollowConfig& config = {},
        const NavQueryOptions& query_options = {});

    [[nodiscard]] NavStatus add_obstacle(
        map_id_t map_id,
        const NavPosition& base_center,
        float radius,
        float height,
        std::uint32_t& obstacle_ref);

    [[nodiscard]] NavStatus add_box_obstacle(
        map_id_t map_id,
        const NavPosition& minimum,
        const NavPosition& maximum,
        std::uint32_t& obstacle_ref);

    [[nodiscard]] NavStatus remove_obstacle(
        map_id_t map_id,
        std::uint32_t obstacle_ref);

    [[nodiscard]] PlayerMoveValidation validate_player_move(
        const HeightMapSystem& height_maps,
        map_id_t map_id,
        const NavPosition& from,
        const NavPosition& client_position,
        std::int8_t last_height_layer,
        float last_server_y,
        const NavQueryOptions& options = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class HeightMapSystem final
{
public:
    // Load and query on the same game thread. Runtime queries are synchronous.
    HeightMapSystem();
    ~HeightMapSystem();

    HeightMapSystem(const HeightMapSystem&) = delete;
    HeightMapSystem& operator=(const HeightMapSystem&) = delete;
    HeightMapSystem(HeightMapSystem&&) noexcept;
    HeightMapSystem& operator=(HeightMapSystem&&) noexcept;

    [[nodiscard]] NavStatus load(map_id_t map_id, const std::filesystem::path& file_path);
    [[nodiscard]] NavStatus load_directory(
        const std::filesystem::path& directory,
        std::size_t& loaded_count);
    [[nodiscard]] bool unload(map_id_t map_id) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool is_loaded(map_id_t map_id) const noexcept;
    [[nodiscard]] NavStatus query(
        map_id_t map_id,
        float x,
        float z,
        float& height) const;
    [[nodiscard]] NavStatus query_all_layers(
        map_id_t map_id,
        float x,
        float z,
        std::vector<float>& layers) const;
    [[nodiscard]] HeightLayerResult validate_layer(
        map_id_t map_id,
        float x,
        float z,
        float client_y,
        float source_x,
        float source_z,
        std::int8_t last_layer,
        float last_y) const;

    [[nodiscard]] std::optional<HeightMapInfo> get_info(map_id_t map_id) const;
    void dump_info(map_id_t map_id) const;
    [[nodiscard]] bool dump_to_file(
        map_id_t map_id,
        const std::filesystem::path& file_path,
        float x_min,
        float z_min,
        float x_max,
        float z_max) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace game::navigation
