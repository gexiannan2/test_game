// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_NAVIGATEMESHHANDLE_H
#define KBE_NAVIGATEMESHHANDLE_H

#include "navigation_handle.h"

#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"

namespace KBEngine {

	struct NavMeshSetHeader
	{
		int version;
		int tileCount;
		dtNavMeshParams params;
	};

	struct NavMeshSetHeaderEx
	{
		int magic;
		int version;
		int tileCount;
		dtNavMeshParams params;
	};

	struct NavMeshTileHeader
	{
		dtTileRef tileRef;
		int dataSize;
	};

	class NavMeshHandle : public NavigationHandle
	{
	public:
		static const int MAX_POLYS = 256;
		static const int NAV_ERROR_NEARESTPOLY = -2;

		static const long RCN_NAVMESH_VERSION = 1;
		static const int INVALID_NAVMESH_POLYREF = 0;

		// 动态阻挡专用 area id（0=默认可行走, 63=动态阻挡）
		static const unsigned char OBSTACLE_AREA_ID = 63;

		struct NavmeshLayer
		{
			dtNavMesh* pNavmesh;
			dtNavMeshQuery* pNavmeshQuery;
		};

		// 障碍物记录
		struct ObstacleRecord
		{
			std::uint32_t ref;                         // 唯一句柄
			std::vector<dtPolyRef> polys;              // 影响的 polygon 列表
			std::vector<unsigned char> origAreas;      // 原始 area id
			std::vector<unsigned short> origFlags;     // 原始 flags（删除时恢复）
			float boundMin[3];                         // 障碍物 AABB min
			float boundMax[3];                         // 障碍物 AABB max
		};

	public:
		NavMeshHandle();
		virtual ~NavMeshHandle();

		int findStraightPath(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& paths);

		int findRandomPointAroundCircle(int layer, const Position3D& centerPos, std::vector<Position3D>& points,
			uint32 max_points, float maxRadius);

		int raycast(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec);

		int GetNearPos(const Position3D& start, Position3D& end, int layer = 0);

		int GetHeight(int layer, const Position3D& pos, float& height);

		// ── 动态阻挡 ──
		int GetMaxObstacles() const override { return kMaxObstacles; }
		int AddObstacle(int layer, const float* center, float radius, float height,
			std::uint32_t& outRef) override;
		int AddBoxObstacle(int layer, const float* bmin, const float* bmax,
			std::uint32_t& outRef) override;
		int RemoveObstacle(int layer, std::uint32_t ref) override;

		// 清空所有动态阻挡记录（障碍物 poly ref 指向的 dtNavMesh 即将被换出时调用，
		// 避免 ref 悬垂到新 dtNavMesh 上）。仅重置记录表，不触碰当前 dtNavMesh。
		void clearObstacles();

		virtual NavigationHandle::NAV_TYPE type() const { return NAV_MESH; }

		static NavigationHandle* create(std::string resPath, const std::map< int, std::string >& params);
		static bool _create(int layer, const std::string& resPath, const std::string& res, NavMeshHandle* pNavMeshHandle);

		std::map<int, NavmeshLayer> navmeshLayer;

	private:
		static const int kMaxObstacles = 256;

		std::vector<ObstacleRecord> obstacles_;
		std::uint32_t nextObstacleRef_ = 1;

		/// 根据 AABB 查询范围内的 polygon 并修改 area 为 OBSTACLE_AREA_ID
		/// @return 0 成功, -1 layer 不存在, -2 查询失败, -3 没有覆盖到任何 poly
		int MarkPolygons(int layer, const float* bmin, const float* bmax,
			std::vector<dtPolyRef>& outPolys,
			std::vector<unsigned char>& outOrigAreas,
			std::vector<unsigned short>* outOrigFlags = nullptr);

		/* Derives overlap polygon of two polygon on the xz-plane.
			@param[in]		polyVertsA		Vertices of polygon A.
			@param[in]		nPolyVertsA		Vertices number of polygon A.
			@param[in]		polyVertsB		Vertices of polygon B.
			@param[in]		nPolyVertsB		Vertices number of polygon B.
			@param[out]		intsectPt		Vertices of overlap polygon.
			@param[out]		intsectPtCount	Vertices number of overlap polygon.
		*/
		void getOverlapPolyPoly2D(const float* polyVertsA, const int nPolyVertsA, const float* polyVertsB, const int nPolyVertsB, float* intsectPt, int* intsectPtCount);

		/* Sort vertices to clockwise. */
		void clockwiseSortPoints(float* verts, const int nVerts);

		/* Determines if two segment cross on xz-plane. */
		bool isSegSegCross2D(const float* p1, const float *p2, const float* q1, const float* q2);

		/// 射线与 AABB 相交检测（slab 算法），用于补 Detour raycast 的动态阻挡盲区
		bool rayIntersectsAABB(const float* origin, const float* dir,
			const float* aabbMin, const float* aabbMax) const;
	};

}

#endif // KBE_NAVIGATEMESHHANDLE_H
