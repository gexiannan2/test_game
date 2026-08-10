/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2016 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef KBE_NAVIGATEGRIDHANDLE_H
#define KBE_NAVIGATEGRIDHANDLE_H

#include "navigation_handle.h"

#include <vector>

#include "stlastar.h"

namespace KBEngine{

class NavGridHandle : public NavigationHandle
{
public:
	static NavGridHandle* pCurrNavGridHandle;
	static int currentLayer;

	static void setMapLayer(int layer)
	{
		currentLayer = layer;
	}

	class MapSearchNode
	{
	public:
		int x;
		int y;

		MapSearchNode() : x(0), y(0) {}
		MapSearchNode(int px, int py) : x(px), y(py) {}

		float GoalDistanceEstimate(MapSearchNode& nodeGoal);
		bool IsGoal(MapSearchNode& nodeGoal);
		bool GetSuccessors(AStarSearch<MapSearchNode>* astarsearch, MapSearchNode* parent_node);
		float GetCost(MapSearchNode& successor);
		bool IsSameState(MapSearchNode& rhs);

		void PrintNodeInfo();
	};

	static MapSearchNode nodeGoal;
	static MapSearchNode nodeStart;
	static AStarSearch<NavGridHandle::MapSearchNode> astarsearch;

public:
	NavGridHandle();
	NavGridHandle(const NavGridHandle& navGridHandle);
	virtual ~NavGridHandle();

	int findStraightPath(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& paths);
	int raycast(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec);
	int raycastNear(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec);
	int raycastAlong(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec);
	int GetHeight(const Position3D& start, float* h, int layer = 0);
	int Intersects(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec);
	int GetNearPos(const Position3D& start, Position3D& end, int layer = 0);

	virtual NavigationHandle::NAV_TYPE type() const{ return NAV_GRID; }

	static NavigationHandle* create(std::string name);

	int getMap(int x, int y) const;
	bool isWalkable(int x, int y) const;
	bool validTile(int x, int y) const;

	void bresenhamLine(const MapSearchNode& p0, const MapSearchNode& p1, std::vector<MapSearchNode>& results) const;
	void bresenhamLine(int x0, int y0, int x1, int y1, std::vector<MapSearchNode>& results) const;

	int width() const { return width_; }
	int height() const { return height_; }

private:
	MapSearchNode worldToGrid(const Position3D& pos) const;
	Position3D gridToWorld(int x, int y, float yCoord) const;
	bool findNearestWalkableTile(const MapSearchNode& start, MapSearchNode& result) const;

private:
	int width_;
	int height_;
	std::vector<uint8> cells_;
};

}
#endif // KBE_NAVIGATEGRIDHANDLE_H
