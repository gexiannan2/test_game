#include "navigation.h"
#include "navigation_handle.h"
#include "navigation_mesh_handle.h"
#include "math/lmath.h"
#include "resmgr/resmgr.h"

#include "e996_capi_nav.h"

using namespace KBEngine;

// 单例实例（由CAPI层管理生命周期）
static Navigation* g_navInstance = nullptr;
static Resmgr*     g_resmgrInstance = nullptr;

// 单例是否已初始化
static bool g_navInitialized = false;

E996_API int e996_nav_init(const char* nav_data_path)
{
    if (g_navInitialized)
        return 0;

    // 创建Resmgr单例（Navigation加载文件时依赖Resmgr）
    g_resmgrInstance = new Resmgr();
    if (nav_data_path)
    {
        g_resmgrInstance->matchPath(nav_data_path);
    }

    // 创建Navigation单例
    g_navInstance = new Navigation();

    g_navInitialized = true;
    return 0;
}

E996_API int e996_nav_load(const char* nav_name)
{
    if (!nav_name || !g_navInitialized)
        return -1;

    auto handle = Navigation::getSingleton().loadNavigation(nav_name);
    return handle ? 0 : -1;
}

E996_API int e996_nav_get_near_pos(const char* nav_name,
    float x, float y, float z,
    float* out_x, float* out_y, float* out_z)
{
    if (!nav_name || !g_navInitialized)
        return -1;

    auto navHandle = Navigation::getSingleton().findNavigation(nav_name);
    if (!navHandle)
        return -1;

    // Position3D: x=x, y=高度(z), z=y（Recast坐标系中y是高度轴）
    Position3D pos(x, z, y);
    Position3D nearPos;

    int ret = navHandle->GetNearPos(pos, nearPos, 0);
    if (ret == NavMeshHandle::NAV_ERROR_NEARESTPOLY)
    {
        return -2;
    }

    if (out_x) *out_x = nearPos.x;
    if (out_y) *out_y = nearPos.z;
    if (out_z) *out_z = nearPos.y;
    return 0;
}

E996_API int e996_nav_find_path(const char* nav_name,
    float start_x, float start_y, float start_z,
    float end_x, float end_y, float end_z,
    float* paths, int* path_count)
{
    if (!nav_name || !paths || !path_count || !g_navInitialized)
        return -1;

    auto navHandle = Navigation::getSingleton().findNavigation(nav_name);
    if (!navHandle)
        return -1;

    Position3D start(start_x, start_z, start_y);
    Position3D end(end_x, end_z, end_y);
    std::vector<Position3D> pathPoints;

    int ret = navHandle->findStraightPath(0, start, end, pathPoints);
    if (ret == NavMeshHandle::NAV_ERROR_NEARESTPOLY)
        return -2;

    int count = 0;
    int max_count = *path_count;
    for (auto& pt : pathPoints)
    {
        if (count >= max_count)
            break;
        paths[count * 3 + 0] = pt.x;
        paths[count * 3 + 1] = pt.z;
        paths[count * 3 + 2] = pt.y;
        count++;
    }
    *path_count = count;
    return 0;
}
