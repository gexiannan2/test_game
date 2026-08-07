// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_NAVIGATEHANDLE_H
#define KBE_NAVIGATEHANDLE_H

#include "common/common.h"

#include "common/smartpointer.h"
#include "common/singleton.h"
#include "math/lmath.h"

namespace KBEngine{


class NavigationHandle : public RefCountable
{
public:
	static const int NAV_ERROR = -1;

	enum NAV_TYPE
	{
		NAV_UNKNOWN = 0,
		NAV_MESH = 1
	};

	enum NAV_OBJECT_STATE
	{
		NAV_OBJECT_STATE_MOVING = 1,	// 移动中
		NAV_OBJECT_STATE_MOVEOVER = 2,	// 移动已经结束了
	};

	NavigationHandle():
	resPath()
	{
	}

	virtual ~NavigationHandle()
	{
	}

	virtual NavigationHandle::NAV_TYPE type() const{ return NAV_UNKNOWN; }

	virtual int findStraightPath(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& paths) = 0;

	virtual int findRandomPointAroundCircle(int layer, const Position3D& centerPos,
		std::vector<Position3D>& points, uint32 max_points, float maxRadius) = 0;

	virtual int raycast(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec) = 0;

	virtual int GetNearPos(const Position3D& start, Position3D& end, int layer = 0) = 0;

	virtual int GetHeight(int layer, const Position3D& pos, float& height) = 0;

	// ── 动态阻挡（setPolyArea 方案）──

	/// 获取最大障碍物数量
	virtual int GetMaxObstacles() const = 0;

	/// 添加圆柱障碍物：在 navmesh 坐标系下指定中心点 (x, height, z)、半径、高度
	/// @return 0 成功，<0 失败，outRef 为句柄（用于删除）
	virtual int AddObstacle(int layer,
		const float* center, float radius, float height,
		std::uint32_t& outRef) = 0;

	/// 添加盒子障碍物（AABB）
	virtual int AddBoxObstacle(int layer,
		const float* bmin, const float* bmax,
		std::uint32_t& outRef) = 0;

	/// 删除障碍物
	virtual int RemoveObstacle(int layer, std::uint32_t ref) = 0;

	std::string resPath;
};

typedef SmartPointer<NavigationHandle> NavigationHandlePtr;

}
#endif // KBE_NAVIGATEHANDLE_H



