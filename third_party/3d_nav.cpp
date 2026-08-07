#include "3d_nav.h"

#include "navigation_mesh_handle.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace game::navigation
{
namespace
{

constexpr float k_obstacle_area_cost = 99999.0f;
constexpr int k_query_node_count = 2048;

struct QueryEndpoints
{
    dtPolyRef start_ref = 0;
    dtPolyRef end_ref = 0;
    NavPosition start{};
    NavPosition end{};
};

struct BuiltPath
{
    NavStatus status = NavStatus::query_failed;
    NavPath path;
    std::vector<dtPolyRef> corridor;
};

struct NavMeshQueryDeleter
{
    void operator()(dtNavMeshQuery* query) const noexcept
    {
        dtFreeNavMeshQuery(query);
    }
};

using NavMeshQueryPtr = std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter>;

[[nodiscard]] bool finite_position(const NavPosition& position) noexcept
{
    return std::isfinite(position.x) &&
        std::isfinite(position.y) &&
        std::isfinite(position.z);
}

[[nodiscard]] bool valid_extents(const NavPosition& extents) noexcept
{
    return finite_position(extents) &&
        extents.x > 0.0f &&
        extents.y > 0.0f &&
        extents.z > 0.0f;
}

[[nodiscard]] bool valid_query_options(const NavQueryOptions& options) noexcept
{
    return valid_extents(options.nearest_poly_extents) &&
        options.max_path_polys > 0 &&
        options.max_path_polys <= 65535 &&
        options.max_straight_path_points > 0 &&
        options.max_straight_path_points <= 65535;
}

[[nodiscard]] std::array<float, 3> to_array(const NavPosition& position) noexcept
{
    return {position.x, position.y, position.z};
}

[[nodiscard]] NavPosition to_position(const float* value) noexcept
{
    return {value[0], value[1], value[2]};
}

void configure_filter(dtQueryFilter& filter) noexcept
{
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);
    filter.setAreaCost(KBEngine::NavMeshHandle::OBSTACLE_AREA_ID, k_obstacle_area_cost);
}

[[nodiscard]] NavStatus find_endpoints(
    dtNavMeshQuery& query,
    const dtQueryFilter& filter,
    const NavPosition& from,
    const NavPosition& to,
    const NavPosition& extents,
    QueryEndpoints& endpoints)
{
    const auto start = to_array(from);
    const auto end = to_array(to);
    const auto search_extents = to_array(extents);
    float nearest_start[3]{};
    float nearest_end[3]{};

    const dtStatus start_status = query.findNearestPoly(
        start.data(),
        search_extents.data(),
        &filter,
        &endpoints.start_ref,
        nearest_start);
    if (dtStatusFailed(start_status))
    {
        return NavStatus::query_failed;
    }
    if (endpoints.start_ref == 0)
    {
        return NavStatus::nearest_poly_not_found;
    }

    const dtStatus end_status = query.findNearestPoly(
        end.data(),
        search_extents.data(),
        &filter,
        &endpoints.end_ref,
        nearest_end);
    if (dtStatusFailed(end_status))
    {
        return NavStatus::query_failed;
    }
    if (endpoints.end_ref == 0)
    {
        return NavStatus::nearest_poly_not_found;
    }

    endpoints.start = to_position(nearest_start);
    endpoints.end = to_position(nearest_end);
    return NavStatus::success;
}

[[nodiscard]] NavStatus build_straight_points(
    dtNavMeshQuery& query,
    const QueryEndpoints& endpoints,
    const std::vector<dtPolyRef>& corridor,
    int max_points,
    bool partial,
    NavPath& result)
{
    result.clear();
    if (corridor.empty() || max_points <= 0)
    {
        return NavStatus::path_not_found;
    }

    auto straight_end = to_array(endpoints.end);
    if (partial)
    {
        const auto requested_end = to_array(endpoints.end);
        const dtStatus closest_status = query.closestPointOnPoly(
            corridor.back(),
            requested_end.data(),
            straight_end.data(),
            nullptr);
        if (dtStatusFailed(closest_status))
        {
            return NavStatus::query_failed;
        }
    }

    const auto straight_start = to_array(endpoints.start);
    std::vector<float> points(static_cast<std::size_t>(max_points) * 3);
    std::vector<unsigned char> point_flags(static_cast<std::size_t>(max_points));
    std::vector<dtPolyRef> point_refs(static_cast<std::size_t>(max_points));
    int point_count = 0;

    const dtStatus straight_status = query.findStraightPath(
        straight_start.data(),
        straight_end.data(),
        corridor.data(),
        static_cast<int>(corridor.size()),
        points.data(),
        point_flags.data(),
        point_refs.data(),
        &point_count,
        max_points);
    if (dtStatusFailed(straight_status))
    {
        return NavStatus::query_failed;
    }
    if (point_count <= 0)
    {
        return NavStatus::path_not_found;
    }

    if (dtStatusDetail(straight_status, DT_BUFFER_TOO_SMALL))
    {
        partial = true;
    }

    result.points.reserve(static_cast<std::size_t>(point_count));
    for (int index = 0; index < point_count; ++index)
    {
        result.points.push_back({
            points[static_cast<std::size_t>(index) * 3],
            points[static_cast<std::size_t>(index) * 3 + 1],
            points[static_cast<std::size_t>(index) * 3 + 2],
        });
    }
    result.partial = partial;
    return partial ? NavStatus::partial_path : NavStatus::success;
}

[[nodiscard]] BuiltPath build_path(
    dtNavMeshQuery& query,
    const dtQueryFilter& filter,
    const NavPosition& from,
    const NavPosition& to,
    const NavQueryOptions& options)
{
    BuiltPath result;
    if (!finite_position(from) || !finite_position(to) || !valid_query_options(options))
    {
        result.status = NavStatus::invalid_argument;
        return result;
    }

    QueryEndpoints endpoints;
    result.status = find_endpoints(
        query,
        filter,
        from,
        to,
        options.nearest_poly_extents,
        endpoints);
    if (result.status != NavStatus::success)
    {
        return result;
    }

    result.corridor.resize(static_cast<std::size_t>(options.max_path_polys));
    int corridor_count = 0;
    const auto start = to_array(endpoints.start);
    const auto end = to_array(endpoints.end);
    const dtStatus path_status = query.findPath(
        endpoints.start_ref,
        endpoints.end_ref,
        start.data(),
        end.data(),
        &filter,
        result.corridor.data(),
        &corridor_count,
        options.max_path_polys);
    if (dtStatusFailed(path_status))
    {
        result.corridor.clear();
        result.status = NavStatus::query_failed;
        return result;
    }
    if (corridor_count <= 0)
    {
        result.corridor.clear();
        result.status = NavStatus::path_not_found;
        return result;
    }
    result.corridor.resize(static_cast<std::size_t>(corridor_count));

    const bool partial =
        result.corridor.back() != endpoints.end_ref ||
        dtStatusDetail(path_status, DT_PARTIAL_RESULT) ||
        dtStatusDetail(path_status, DT_BUFFER_TOO_SMALL);
    result.status = build_straight_points(
        query,
        endpoints,
        result.corridor,
        options.max_straight_path_points,
        partial,
        result.path);
    return result;
}

[[nodiscard]] float horizontal_distance_squared(
    const NavPosition& lhs,
    const NavPosition& rhs) noexcept
{
    const float dx = lhs.x - rhs.x;
    const float dz = lhs.z - rhs.z;
    return dx * dx + dz * dz;
}

[[nodiscard]] float distance_to_segment_squared(
    const NavPosition& point,
    const NavPosition& start,
    const NavPosition& end) noexcept
{
    const float segment_x = end.x - start.x;
    const float segment_z = end.z - start.z;
    const float length_squared = segment_x * segment_x + segment_z * segment_z;
    if (length_squared <= std::numeric_limits<float>::epsilon())
    {
        return horizontal_distance_squared(point, start);
    }

    const float point_x = point.x - start.x;
    const float point_z = point.z - start.z;
    const float factor = std::clamp(
        (point_x * segment_x + point_z * segment_z) / length_squared,
        0.0f,
        1.0f);
    const NavPosition closest{
        start.x + segment_x * factor,
        point.y,
        start.z + segment_z * factor,
    };
    return horizontal_distance_squared(point, closest);
}

[[nodiscard]] std::optional<map_id_t> parse_map_id(
    const std::filesystem::path& file_path) noexcept
{
    const std::string stem = file_path.stem().string();
    if (stem.empty())
    {
        return std::nullopt;
    }

    std::uint32_t value = 0;
    const char* begin = stem.data();
    const char* end = begin;
    while (end != begin + stem.size() && *end >= '0' && *end <= '9')
    {
        ++end;
    }
    if (end == begin)
    {
        return std::nullopt;
    }

    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || parsed_end != end)
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] NavStatus map_obstacle_status(int status) noexcept
{
    switch (status)
    {
    case 0:
        return NavStatus::success;
    case -1:
        return NavStatus::layer_not_found;
    case -2:
        return NavStatus::query_not_found;
    case -4:
        return NavStatus::obstacle_limit_reached;
    default:
        return NavStatus::query_failed;
    }
}

} // namespace

struct NavSystem::Impl
{
    struct MapEntry
    {
        std::unique_ptr<NavMeshHandle> navmesh;
        int layer = 0;
        dtQueryFilter filter;
    };

    struct SlicedState
    {
        map_id_t map_id = 0;
        NavMeshQueryPtr query;
        QueryEndpoints endpoints;
        NavQueryOptions options;
        bool ready = false;
    };

    std::unordered_map<map_id_t, MapEntry> maps;
    std::unordered_map<std::uint64_t, SlicedState> sliced_queries;
    std::uint64_t next_sliced_id = 1;

    [[nodiscard]] MapEntry* find_map(map_id_t map_id) noexcept
    {
        const auto iterator = maps.find(map_id);
        return iterator == maps.end() ? nullptr : &iterator->second;
    }

    [[nodiscard]] const MapEntry* find_map(map_id_t map_id) const noexcept
    {
        const auto iterator = maps.find(map_id);
        return iterator == maps.end() ? nullptr : &iterator->second;
    }

    [[nodiscard]] static KBEngine::NavMeshHandle::NavmeshLayer* find_layer(
        MapEntry& entry) noexcept
    {
        const auto iterator = entry.navmesh->navmeshLayer.find(entry.layer);
        return iterator == entry.navmesh->navmeshLayer.end() ? nullptr : &iterator->second;
    }

    [[nodiscard]] static const KBEngine::NavMeshHandle::NavmeshLayer* find_layer(
        const MapEntry& entry) noexcept
    {
        const auto iterator = entry.navmesh->navmeshLayer.find(entry.layer);
        return iterator == entry.navmesh->navmeshLayer.end() ? nullptr : &iterator->second;
    }

    void cancel_sliced_for_map(map_id_t map_id) noexcept
    {
        for (auto iterator = sliced_queries.begin(); iterator != sliced_queries.end();)
        {
            if (iterator->second.map_id == map_id)
            {
                iterator = sliced_queries.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }

    [[nodiscard]] std::uint64_t allocate_sliced_id() noexcept
    {
        while (next_sliced_id == 0 || sliced_queries.contains(next_sliced_id))
        {
            ++next_sliced_id;
        }
        return next_sliced_id++;
    }

    [[nodiscard]] NavStatus finalize_sliced(
        SlicedPathQuery& public_query,
        NavPath& result,
        std::vector<std::uint64_t>* corridor_refs)
    {
        result.clear();
        const auto iterator = sliced_queries.find(public_query.id);
        if (iterator == sliced_queries.end() || iterator->second.map_id != public_query.map_id)
        {
            public_query.id = 0;
            public_query.state = SlicedPathState::cancelled;
            public_query.status = NavStatus::query_not_found;
            return public_query.status;
        }

        SlicedState& state = iterator->second;
        if (!state.ready)
        {
            public_query.state = SlicedPathState::in_progress;
            public_query.status = NavStatus::in_progress;
            return public_query.status;
        }

        std::vector<dtPolyRef> corridor(
            static_cast<std::size_t>(state.options.max_path_polys));
        int corridor_count = 0;
        const dtStatus finalize_status = state.query->finalizeSlicedFindPath(
            corridor.data(),
            &corridor_count,
            state.options.max_path_polys);
        if (dtStatusFailed(finalize_status) || corridor_count <= 0)
        {
            sliced_queries.erase(iterator);
            public_query.id = 0;
            public_query.state = SlicedPathState::failed;
            public_query.status = corridor_count <= 0
                ? NavStatus::path_not_found
                : NavStatus::query_failed;
            return public_query.status;
        }
        corridor.resize(static_cast<std::size_t>(corridor_count));

        const bool partial =
            corridor.back() != state.endpoints.end_ref ||
            dtStatusDetail(finalize_status, DT_PARTIAL_RESULT) ||
            dtStatusDetail(finalize_status, DT_BUFFER_TOO_SMALL);
        const NavStatus result_status = build_straight_points(
            *state.query,
            state.endpoints,
            corridor,
            state.options.max_straight_path_points,
            partial,
            result);

        if (corridor_refs != nullptr)
        {
            corridor_refs->clear();
            corridor_refs->reserve(corridor.size());
            for (const dtPolyRef reference : corridor)
            {
                corridor_refs->push_back(static_cast<std::uint64_t>(reference));
            }
        }

        sliced_queries.erase(iterator);
        public_query.id = 0;
        public_query.state = nav_status_has_path(result_status)
            ? SlicedPathState::finalized
            : SlicedPathState::failed;
        public_query.status = result_status;
        return result_status;
    }

    [[nodiscard]] bool corridor_valid(
        map_id_t map_id,
        const std::vector<std::uint64_t>& corridor) const noexcept
    {
        const MapEntry* entry = find_map(map_id);
        if (entry == nullptr)
        {
            return false;
        }
        const auto* layer = find_layer(*entry);
        if (layer == nullptr || layer->pNavmeshQuery == nullptr)
        {
            return false;
        }
        for (const std::uint64_t raw_reference : corridor)
        {
            const auto reference = static_cast<dtPolyRef>(raw_reference);
            if (!layer->pNavmeshQuery->isValidPolyRef(reference, &entry->filter))
            {
                return false;
            }
        }
        return true;
    }
};

std::string_view to_string(NavStatus status) noexcept
{
    switch (status)
    {
    case NavStatus::success:
        return "success";
    case NavStatus::in_progress:
        return "in_progress";
    case NavStatus::partial_path:
        return "partial_path";
    case NavStatus::invalid_argument:
        return "invalid_argument";
    case NavStatus::navmesh_not_loaded:
        return "navmesh_not_loaded";
    case NavStatus::heightmap_not_loaded:
        return "heightmap_not_loaded";
    case NavStatus::layer_not_found:
        return "layer_not_found";
    case NavStatus::nearest_poly_not_found:
        return "nearest_poly_not_found";
    case NavStatus::path_not_found:
        return "path_not_found";
    case NavStatus::query_failed:
        return "query_failed";
    case NavStatus::query_cancelled:
        return "query_cancelled";
    case NavStatus::query_not_found:
        return "query_not_found";
    case NavStatus::out_of_range:
        return "out_of_range";
    case NavStatus::height_mismatch:
        return "height_mismatch";
    case NavStatus::obstacle_limit_reached:
        return "obstacle_limit_reached";
    }
    return "unknown";
}

void FollowPath::reset() noexcept
{
    state = FollowState::idle;
    last_status = NavStatus::success;
    path.clear();
    waypoint_index = 0;
    corridor_refs.clear();
    target = {};
    last_repath_target = {};
    seconds_since_repath = 0.0f;
    consecutive_failures = 0;
    pending_path = {};
}

void NavigationBudget::reset() noexcept
{
    used_full_path_queries = 0;
    used_sliced_iterations = 0;
}

bool NavigationBudget::try_consume_full_path() noexcept
{
    if (used_full_path_queries >= max_full_path_queries)
    {
        return false;
    }
    ++used_full_path_queries;
    return true;
}

int NavigationBudget::consume_sliced_iterations(int requested) noexcept
{
    if (requested <= 0 || max_sliced_iterations <= used_sliced_iterations)
    {
        return 0;
    }
    const int available = max_sliced_iterations - used_sliced_iterations;
    const int granted = std::min(requested, available);
    used_sliced_iterations += granted;
    return granted;
}

NavSystem::NavSystem()
    : impl_(std::make_unique<Impl>())
{
}

NavSystem::~NavSystem() = default;
NavSystem::NavSystem(NavSystem&&) noexcept = default;
NavSystem& NavSystem::operator=(NavSystem&&) noexcept = default;

NavStatus NavSystem::load_navmesh(
    map_id_t map_id,
    const std::filesystem::path& file_path)
{
    if (map_id > static_cast<map_id_t>(std::numeric_limits<int>::max()) ||
        file_path.empty())
    {
        return NavStatus::invalid_argument;
    }

    std::error_code error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(file_path, error);
    if (error || !std::filesystem::is_regular_file(absolute_path, error) || error)
    {
        return NavStatus::query_failed;
    }

    auto handle = std::make_unique<NavMeshHandle>();
    const int layer = static_cast<int>(map_id);
    if (!NavMeshHandle::_create(
            layer,
            absolute_path.parent_path().string(),
            absolute_path.string(),
            handle.get()))
    {
        return NavStatus::query_failed;
    }

    Impl::MapEntry entry;
    entry.navmesh = std::move(handle);
    entry.layer = layer;
    configure_filter(entry.filter);

    impl_->cancel_sliced_for_map(map_id);
    impl_->maps.insert_or_assign(map_id, std::move(entry));
    return NavStatus::success;
}

NavStatus NavSystem::load_navmesh_directory(
    const std::filesystem::path& directory,
    std::size_t& loaded_count)
{
    loaded_count = 0;
    if (directory.empty())
    {
        return NavStatus::invalid_argument;
    }

    std::error_code error;
    const std::filesystem::path absolute_directory =
        std::filesystem::absolute(directory, error);
    if (error || !std::filesystem::is_directory(absolute_directory, error) || error)
    {
        return NavStatus::query_failed;
    }

    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator iterator(absolute_directory, error), end;
         !error && iterator != end;
         iterator.increment(error))
    {
        if (iterator->is_regular_file(error) &&
            iterator->path().extension() == ".navmesh")
        {
            files.push_back(iterator->path());
        }
    }
    if (error)
    {
        return NavStatus::query_failed;
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files)
    {
        const auto map_id = parse_map_id(file);
        if (!map_id.has_value())
        {
            continue;
        }
        if (load_navmesh(*map_id, file) == NavStatus::success)
        {
            ++loaded_count;
        }
    }
    return loaded_count > 0 ? NavStatus::success : NavStatus::query_failed;
}

bool NavSystem::unload_navmesh(map_id_t map_id) noexcept
{
    impl_->cancel_sliced_for_map(map_id);
    return impl_->maps.erase(map_id) > 0;
}

void NavSystem::clear() noexcept
{
    impl_->sliced_queries.clear();
    impl_->maps.clear();
}

bool NavSystem::is_loaded(map_id_t map_id) const noexcept
{
    return impl_->find_map(map_id) != nullptr;
}

NavMeshHandle* NavSystem::get_navmesh(map_id_t map_id) noexcept
{
    Impl::MapEntry* entry = impl_->find_map(map_id);
    return entry == nullptr ? nullptr : entry->navmesh.get();
}

const NavMeshHandle* NavSystem::get_navmesh(map_id_t map_id) const noexcept
{
    const Impl::MapEntry* entry = impl_->find_map(map_id);
    return entry == nullptr ? nullptr : entry->navmesh.get();
}

NavStatus NavSystem::find_nearest_position(
    map_id_t map_id,
    const NavPosition& position,
    NavPosition& nearest,
    const NavPosition& extents) const
{
    if (!finite_position(position) || !valid_extents(extents))
    {
        return NavStatus::invalid_argument;
    }
    const Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        return NavStatus::navmesh_not_loaded;
    }
    const auto* layer = Impl::find_layer(*entry);
    if (layer == nullptr || layer->pNavmeshQuery == nullptr)
    {
        return NavStatus::layer_not_found;
    }

    const auto source = to_array(position);
    const auto search_extents = to_array(extents);
    dtPolyRef reference = 0;
    float nearest_point[3]{};
    const dtStatus status = layer->pNavmeshQuery->findNearestPoly(
        source.data(),
        search_extents.data(),
        &entry->filter,
        &reference,
        nearest_point);
    if (dtStatusFailed(status))
    {
        return NavStatus::query_failed;
    }
    if (reference == 0)
    {
        return NavStatus::nearest_poly_not_found;
    }
    nearest = to_position(nearest_point);
    return NavStatus::success;
}

NavStatus NavSystem::find_straight_path(
    map_id_t map_id,
    const NavPosition& from,
    const NavPosition& to,
    NavPath& result,
    const NavQueryOptions& options) const
{
    result.clear();
    const Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        return NavStatus::navmesh_not_loaded;
    }
    const auto* layer = Impl::find_layer(*entry);
    if (layer == nullptr || layer->pNavmeshQuery == nullptr)
    {
        return NavStatus::layer_not_found;
    }

    BuiltPath built = build_path(
        *layer->pNavmeshQuery,
        entry->filter,
        from,
        to,
        options);
    result = std::move(built.path);
    return built.status;
}

ReachabilityResult NavSystem::validate_reachable(
    map_id_t map_id,
    const NavPosition& from,
    const NavPosition& to,
    const NavQueryOptions& options) const
{
    ReachabilityResult result;
    result.position = from;
    if (!finite_position(from) || !finite_position(to) || !valid_query_options(options))
    {
        result.status = NavStatus::invalid_argument;
        return result;
    }

    const Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        result.status = NavStatus::navmesh_not_loaded;
        return result;
    }
    const auto* layer = Impl::find_layer(*entry);
    if (layer == nullptr || layer->pNavmeshQuery == nullptr)
    {
        result.status = NavStatus::layer_not_found;
        return result;
    }

    QueryEndpoints endpoints;
    result.status = find_endpoints(
        *layer->pNavmeshQuery,
        entry->filter,
        from,
        to,
        options.nearest_poly_extents,
        endpoints);
    if (result.status != NavStatus::success)
    {
        return result;
    }

    const auto start = to_array(endpoints.start);
    const auto end = to_array(endpoints.end);
    std::vector<dtPolyRef> visited(static_cast<std::size_t>(options.max_path_polys));
    int visited_count = 0;
    float hit_fraction = 0.0f;
    float hit_normal[3]{};
    const dtStatus raycast_status = layer->pNavmeshQuery->raycast(
        endpoints.start_ref,
        start.data(),
        end.data(),
        &entry->filter,
        &hit_fraction,
        hit_normal,
        visited.data(),
        &visited_count,
        options.max_path_polys);
    if (dtStatusFailed(raycast_status))
    {
        result.status = NavStatus::query_failed;
        return result;
    }

    if (hit_fraction >= 1.0f)
    {
        result.status = NavStatus::success;
        result.reachable = true;
        result.position = endpoints.end;
        return result;
    }

    BuiltPath detour = build_path(
        *layer->pNavmeshQuery,
        entry->filter,
        from,
        to,
        options);
    result.status = detour.status;
    result.reachable = detour.status == NavStatus::success;
    if (result.reachable)
    {
        result.position = endpoints.end;
    }
    else if (!detour.path.points.empty())
    {
        result.position = detour.path.points.back();
    }
    return result;
}

NavStatus NavSystem::get_poly_height(
    map_id_t map_id,
    const NavPosition& position,
    float& height) const
{
    if (!finite_position(position))
    {
        return NavStatus::invalid_argument;
    }
    const Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        return NavStatus::navmesh_not_loaded;
    }
    const auto* layer = Impl::find_layer(*entry);
    if (layer == nullptr || layer->pNavmeshQuery == nullptr)
    {
        return NavStatus::layer_not_found;
    }

    QueryEndpoints endpoints;
    const NavStatus endpoint_status = find_endpoints(
        *layer->pNavmeshQuery,
        entry->filter,
        position,
        position,
        {2.0f, 4.0f, 2.0f},
        endpoints);
    if (endpoint_status != NavStatus::success)
    {
        return endpoint_status;
    }
    const auto source = to_array(position);
    const dtStatus status = layer->pNavmeshQuery->getPolyHeight(
        endpoints.start_ref,
        source.data(),
        &height);
    return dtStatusFailed(status) ? NavStatus::query_failed : NavStatus::success;
}

NavStatus NavSystem::begin_sliced_path(
    map_id_t map_id,
    const NavPosition& from,
    const NavPosition& to,
    SlicedPathQuery& public_query,
    const NavQueryOptions& options)
{
    cancel_sliced_path(public_query);
    if (!finite_position(from) || !finite_position(to) || !valid_query_options(options))
    {
        public_query.state = SlicedPathState::failed;
        public_query.status = NavStatus::invalid_argument;
        return public_query.status;
    }

    Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        public_query.state = SlicedPathState::failed;
        public_query.status = NavStatus::navmesh_not_loaded;
        return public_query.status;
    }
    auto* layer = Impl::find_layer(*entry);
    if (layer == nullptr || layer->pNavmesh == nullptr)
    {
        public_query.state = SlicedPathState::failed;
        public_query.status = NavStatus::layer_not_found;
        return public_query.status;
    }

    Impl::SlicedState state;
    state.map_id = map_id;
    state.query.reset(dtAllocNavMeshQuery());
    if (!state.query)
    {
        public_query.state = SlicedPathState::failed;
        public_query.status = NavStatus::query_failed;
        return public_query.status;
    }
    if (dtStatusFailed(state.query->init(layer->pNavmesh, k_query_node_count)))
    {
        public_query.state = SlicedPathState::failed;
        public_query.status = NavStatus::query_failed;
        return public_query.status;
    }

    state.options = options;
    const NavStatus endpoint_status = find_endpoints(
        *state.query,
        entry->filter,
        from,
        to,
        options.nearest_poly_extents,
        state.endpoints);
    if (endpoint_status != NavStatus::success)
    {
        public_query.state = SlicedPathState::failed;
        public_query.status = endpoint_status;
        return public_query.status;
    }

    const auto start = to_array(state.endpoints.start);
    const auto end = to_array(state.endpoints.end);
    const dtStatus begin_status = state.query->initSlicedFindPath(
        state.endpoints.start_ref,
        state.endpoints.end_ref,
        start.data(),
        end.data(),
        &entry->filter);
    if (dtStatusFailed(begin_status))
    {
        public_query.state = SlicedPathState::failed;
        public_query.status = NavStatus::query_failed;
        return public_query.status;
    }

    state.ready = dtStatusSucceed(begin_status);
    const std::uint64_t id = impl_->allocate_sliced_id();
    impl_->sliced_queries.emplace(id, std::move(state));
    public_query.id = id;
    public_query.map_id = map_id;
    public_query.state = dtStatusInProgress(begin_status)
        ? SlicedPathState::in_progress
        : SlicedPathState::ready;
    public_query.status = dtStatusInProgress(begin_status)
        ? NavStatus::in_progress
        : NavStatus::success;
    return public_query.status;
}

NavStatus NavSystem::update_sliced_path(
    SlicedPathQuery& public_query,
    int max_iterations,
    int& completed_iterations)
{
    completed_iterations = 0;
    if (max_iterations <= 0)
    {
        return NavStatus::invalid_argument;
    }
    const auto iterator = impl_->sliced_queries.find(public_query.id);
    if (iterator == impl_->sliced_queries.end() ||
        iterator->second.map_id != public_query.map_id)
    {
        public_query.id = 0;
        public_query.state = SlicedPathState::cancelled;
        public_query.status = NavStatus::query_not_found;
        return public_query.status;
    }
    if (iterator->second.ready)
    {
        public_query.state = SlicedPathState::ready;
        public_query.status = NavStatus::success;
        return public_query.status;
    }

    const dtStatus status = iterator->second.query->updateSlicedFindPath(
        max_iterations,
        &completed_iterations);
    if (dtStatusFailed(status))
    {
        impl_->sliced_queries.erase(iterator);
        public_query.id = 0;
        public_query.state = SlicedPathState::failed;
        public_query.status = NavStatus::query_failed;
        return public_query.status;
    }
    if (dtStatusInProgress(status))
    {
        public_query.state = SlicedPathState::in_progress;
        public_query.status = NavStatus::in_progress;
        return public_query.status;
    }

    iterator->second.ready = true;
    public_query.state = SlicedPathState::ready;
    public_query.status = NavStatus::success;
    return public_query.status;
}

NavStatus NavSystem::finalize_sliced_path(
    SlicedPathQuery& query,
    NavPath& result)
{
    return impl_->finalize_sliced(query, result, nullptr);
}

void NavSystem::cancel_sliced_path(SlicedPathQuery& query) noexcept
{
    if (query.id != 0)
    {
        impl_->sliced_queries.erase(query.id);
    }
    query.id = 0;
    query.state = SlicedPathState::cancelled;
    query.status = NavStatus::query_cancelled;
}

void NavSystem::start_follow(
    FollowPath& follow,
    const NavPosition& target) noexcept
{
    cancel_sliced_path(follow.pending_path);
    follow.reset();
    follow.target = target;
    follow.last_repath_target = target;
    follow.seconds_since_repath = std::numeric_limits<float>::max();
    follow.state = FollowState::repath_required;
}

void NavSystem::stop_follow(FollowPath& follow) noexcept
{
    cancel_sliced_path(follow.pending_path);
    follow.reset();
}

FollowUpdate NavSystem::update_follow(
    map_id_t map_id,
    FollowPath& follow,
    const NavPosition& current_position,
    const NavPosition& target,
    float delta_seconds,
    NavigationBudget& budget,
    const FollowConfig& config,
    const NavQueryOptions& query_options)
{
    FollowUpdate update;
    update.state = follow.state;
    update.status = follow.last_status;

    if (!finite_position(current_position) ||
        !finite_position(target) ||
        !std::isfinite(delta_seconds) ||
        delta_seconds < 0.0f ||
        config.target_move_threshold <= 0.0f ||
        config.waypoint_reach_distance <= 0.0f ||
        config.arrival_distance <= 0.0f ||
        config.path_deviation_threshold <= 0.0f ||
        config.repath_interval_seconds < 0.0f ||
        config.sliced_iterations_per_tick <= 0 ||
        config.max_repath_failures == 0 ||
        !valid_query_options(query_options))
    {
        cancel_sliced_path(follow.pending_path);
        follow.state = FollowState::failed;
        follow.last_status = NavStatus::invalid_argument;
        update.state = follow.state;
        update.status = follow.last_status;
        return update;
    }
    if (!is_loaded(map_id))
    {
        cancel_sliced_path(follow.pending_path);
        follow.state = FollowState::failed;
        follow.last_status = NavStatus::navmesh_not_loaded;
        update.state = follow.state;
        update.status = follow.last_status;
        return update;
    }

    follow.target = target;
    follow.seconds_since_repath += delta_seconds;

    const float arrival_distance_squared =
        config.arrival_distance * config.arrival_distance;
    if (horizontal_distance_squared(current_position, target) <= arrival_distance_squared)
    {
        cancel_sliced_path(follow.pending_path);
        follow.state = FollowState::arrived;
        follow.last_status = NavStatus::success;
        follow.path.clear();
        follow.corridor_refs.clear();
        update.state = follow.state;
        update.status = follow.last_status;
        return update;
    }

    const float target_move_threshold_squared =
        config.target_move_threshold * config.target_move_threshold;
    const bool target_moved =
        horizontal_distance_squared(target, follow.last_repath_target) >=
        target_move_threshold_squared;

    if (follow.state == FollowState::idle ||
        follow.state == FollowState::arrived ||
        follow.state == FollowState::failed)
    {
        follow.state = FollowState::repath_required;
    }

    if (follow.state == FollowState::path_pending && target_moved)
    {
        cancel_sliced_path(follow.pending_path);
        follow.state = FollowState::repath_required;
    }

    if (follow.state == FollowState::path_pending)
    {
        const int iterations = budget.consume_sliced_iterations(
            config.sliced_iterations_per_tick);
        if (iterations > 0)
        {
            int completed = 0;
            const NavStatus sliced_status =
                update_sliced_path(follow.pending_path, iterations, completed);
            update.repath_started = true;
            if (sliced_status == NavStatus::success &&
                follow.pending_path.state == SlicedPathState::ready)
            {
                NavPath new_path;
                std::vector<std::uint64_t> new_corridor;
                const NavStatus finalize_status = impl_->finalize_sliced(
                    follow.pending_path,
                    new_path,
                    &new_corridor);
                if (finalize_status == NavStatus::success)
                {
                    follow.path = std::move(new_path);
                    follow.corridor_refs = std::move(new_corridor);
                    follow.waypoint_index = 0;
                    follow.consecutive_failures = 0;
                    follow.state = FollowState::following;
                    follow.last_status = NavStatus::success;
                    update.path_replaced = true;
                }
                else
                {
                    ++follow.consecutive_failures;
                    follow.last_status = finalize_status;
                    follow.state =
                        follow.consecutive_failures >= config.max_repath_failures
                        ? FollowState::failed
                        : FollowState::repath_required;
                }
            }
            else if (sliced_status != NavStatus::in_progress &&
                     sliced_status != NavStatus::success)
            {
                ++follow.consecutive_failures;
                follow.last_status = sliced_status;
                follow.state =
                    follow.consecutive_failures >= config.max_repath_failures
                    ? FollowState::failed
                    : FollowState::repath_required;
            }
        }
    }

    if (follow.state == FollowState::following ||
        follow.state == FollowState::repath_required)
    {
        const bool invalid_corridor =
            !follow.corridor_refs.empty() &&
            !impl_->corridor_valid(map_id, follow.corridor_refs);

        bool deviated = false;
        if (!follow.path.points.empty() &&
            follow.waypoint_index < follow.path.points.size())
        {
            const NavPosition& segment_end =
                follow.path.points[follow.waypoint_index];
            const NavPosition& segment_start =
                follow.waypoint_index > 0
                ? follow.path.points[follow.waypoint_index - 1]
                : current_position;
            const float deviation_squared =
                config.path_deviation_threshold * config.path_deviation_threshold;
            deviated = distance_to_segment_squared(
                current_position,
                segment_start,
                segment_end) > deviation_squared;
        }

        if (invalid_corridor || deviated ||
            (target_moved &&
             follow.seconds_since_repath >= config.repath_interval_seconds))
        {
            follow.state = FollowState::repath_required;
        }
    }

    if (follow.state == FollowState::repath_required &&
        follow.seconds_since_repath >= config.repath_interval_seconds)
    {
        if (config.use_sliced_path)
        {
            const NavStatus begin_status = begin_sliced_path(
                map_id,
                current_position,
                target,
                follow.pending_path,
                query_options);
            follow.last_status = begin_status;
            follow.last_repath_target = target;
            follow.seconds_since_repath = 0.0f;
            update.repath_started = true;
            if (begin_status == NavStatus::in_progress)
            {
                follow.state = FollowState::path_pending;
            }
            else if (begin_status == NavStatus::success)
            {
                follow.state = FollowState::path_pending;
            }
            else
            {
                ++follow.consecutive_failures;
                follow.state =
                    follow.consecutive_failures >= config.max_repath_failures
                    ? FollowState::failed
                    : FollowState::repath_required;
            }
        }
        else if (budget.try_consume_full_path())
        {
            const Impl::MapEntry* entry = impl_->find_map(map_id);
            const auto* layer = entry == nullptr ? nullptr : Impl::find_layer(*entry);
            BuiltPath built;
            if (entry == nullptr)
            {
                built.status = NavStatus::navmesh_not_loaded;
            }
            else if (layer == nullptr || layer->pNavmeshQuery == nullptr)
            {
                built.status = NavStatus::layer_not_found;
            }
            else
            {
                built = build_path(
                    *layer->pNavmeshQuery,
                    entry->filter,
                    current_position,
                    target,
                    query_options);
            }

            follow.last_repath_target = target;
            follow.seconds_since_repath = 0.0f;
            follow.last_status = built.status;
            update.repath_started = true;
            if (built.status == NavStatus::success)
            {
                follow.path = std::move(built.path);
                follow.corridor_refs.clear();
                follow.corridor_refs.reserve(built.corridor.size());
                for (const dtPolyRef reference : built.corridor)
                {
                    follow.corridor_refs.push_back(
                        static_cast<std::uint64_t>(reference));
                }
                follow.waypoint_index = 0;
                follow.consecutive_failures = 0;
                follow.state = FollowState::following;
                update.path_replaced = true;
            }
            else
            {
                ++follow.consecutive_failures;
                follow.state =
                    follow.consecutive_failures >= config.max_repath_failures
                    ? FollowState::failed
                    : FollowState::repath_required;
            }
        }
    }

    if ((follow.state == FollowState::following ||
         follow.state == FollowState::repath_required ||
         follow.state == FollowState::path_pending) &&
        !follow.path.points.empty())
    {
        const float waypoint_distance_squared =
            config.waypoint_reach_distance * config.waypoint_reach_distance;
        while (follow.waypoint_index < follow.path.points.size() &&
               horizontal_distance_squared(
                   current_position,
                   follow.path.points[follow.waypoint_index]) <=
                   waypoint_distance_squared)
        {
            ++follow.waypoint_index;
        }

        if (follow.waypoint_index < follow.path.points.size())
        {
            update.has_steering_target = true;
            update.steering_target = follow.path.points[follow.waypoint_index];
        }
        else if (horizontal_distance_squared(current_position, target) <=
                 arrival_distance_squared)
        {
            follow.state = FollowState::arrived;
            follow.last_status = NavStatus::success;
        }
        else
        {
            follow.state = FollowState::repath_required;
        }
    }

    update.state = follow.state;
    update.status = follow.last_status;
    return update;
}

NavStatus NavSystem::add_obstacle(
    map_id_t map_id,
    const NavPosition& base_center,
    float radius,
    float height,
    std::uint32_t& obstacle_ref)
{
    obstacle_ref = 0;
    if (!finite_position(base_center) ||
        !std::isfinite(radius) ||
        !std::isfinite(height) ||
        radius <= 0.0f ||
        height <= 0.0f)
    {
        return NavStatus::invalid_argument;
    }
    Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        return NavStatus::navmesh_not_loaded;
    }

    impl_->cancel_sliced_for_map(map_id);
    const auto center = to_array(base_center);
    return map_obstacle_status(entry->navmesh->AddObstacle(
        entry->layer,
        center.data(),
        radius,
        height,
        obstacle_ref));
}

NavStatus NavSystem::add_box_obstacle(
    map_id_t map_id,
    const NavPosition& minimum,
    const NavPosition& maximum,
    std::uint32_t& obstacle_ref)
{
    obstacle_ref = 0;
    if (!finite_position(minimum) ||
        !finite_position(maximum) ||
        minimum.x >= maximum.x ||
        minimum.y >= maximum.y ||
        minimum.z >= maximum.z)
    {
        return NavStatus::invalid_argument;
    }
    Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        return NavStatus::navmesh_not_loaded;
    }

    impl_->cancel_sliced_for_map(map_id);
    const auto bounds_minimum = to_array(minimum);
    const auto bounds_maximum = to_array(maximum);
    return map_obstacle_status(entry->navmesh->AddBoxObstacle(
        entry->layer,
        bounds_minimum.data(),
        bounds_maximum.data(),
        obstacle_ref));
}

NavStatus NavSystem::remove_obstacle(
    map_id_t map_id,
    std::uint32_t obstacle_ref)
{
    if (obstacle_ref == 0)
    {
        return NavStatus::invalid_argument;
    }
    Impl::MapEntry* entry = impl_->find_map(map_id);
    if (entry == nullptr)
    {
        return NavStatus::navmesh_not_loaded;
    }

    impl_->cancel_sliced_for_map(map_id);
    return map_obstacle_status(
        entry->navmesh->RemoveObstacle(entry->layer, obstacle_ref));
}

PlayerMoveValidation NavSystem::validate_player_move(
    const HeightMapSystem& height_maps,
    map_id_t map_id,
    const NavPosition& from,
    const NavPosition& client_position,
    std::int8_t last_height_layer,
    float last_server_y,
    const NavQueryOptions& options) const
{
    PlayerMoveValidation result;
    result.server_position = from;

    const ReachabilityResult reachable =
        validate_reachable(map_id, from, client_position, options);
    if (reachable.status != NavStatus::success || !reachable.reachable)
    {
        result.status = reachable.status == NavStatus::success
            ? NavStatus::path_not_found
            : reachable.status;
        return result;
    }

    const HeightLayerResult height = height_maps.validate_layer(
        map_id,
        client_position.x,
        client_position.z,
        client_position.y,
        from.x,
        from.z,
        last_height_layer,
        last_server_y);
    if (height.status != NavStatus::success)
    {
        result.status = height.status;
        return result;
    }

    result.status = NavStatus::success;
    result.server_position = client_position;
    result.server_position.y = height.height;
    result.height_layer = height.layer;
    result.layer_switched = height.switched;
    return result;
}

} // namespace game::navigation
