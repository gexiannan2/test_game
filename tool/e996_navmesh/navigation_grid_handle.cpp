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

#include "navigation_grid_handle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>

#include "navigation.h"
#include "resmgr/resmgr.h"

namespace KBEngine{

NavGridHandle* NavGridHandle::pCurrNavGridHandle = NULL;
int NavGridHandle::currentLayer = 0;
NavGridHandle::MapSearchNode NavGridHandle::nodeGoal;
NavGridHandle::MapSearchNode NavGridHandle::nodeStart;
AStarSearch<NavGridHandle::MapSearchNode> NavGridHandle::astarsearch;

namespace
{
inline int32 readInt32LE(const uint8* data)
{
	return (int32)(
		((uint32)data[0]) |
		((uint32)data[1] << 8) |
		((uint32)data[2] << 16) |
		((uint32)data[3] << 24));
}

inline void swapInt(int& a, int& b)
{
	int c = a;
	a = b;
	b = c;
}

inline float diagonalCost(bool diagonal)
{
	return diagonal ? 1.41421356f : 1.0f;
}
}

NavGridHandle::NavGridHandle():
NavigationHandle(),
width_(0),
height_(0),
cells_()
{
}

NavGridHandle::NavGridHandle(const NavGridHandle& navGridHandle):
NavigationHandle(),
width_(navGridHandle.width_),
height_(navGridHandle.height_),
cells_(navGridHandle.cells_)
{
	name = navGridHandle.name;
}

NavGridHandle::~NavGridHandle()
{
}

NavGridHandle::MapSearchNode NavGridHandle::worldToGrid(const Position3D& pos) const
{
	return MapSearchNode((int)floorf(pos.x), (int)floorf(pos.z));
}

Position3D NavGridHandle::gridToWorld(int x, int y, float yCoord) const
{
	return Position3D((float)x, yCoord, (float)y);
}

bool NavGridHandle::validTile(int x, int y) const
{
	return x >= 0 && x < width_ && y >= 0 && y < height_;
}

int NavGridHandle::getMap(int x, int y) const
{
	if(!validTile(x, y))
		return 1;

	return (int)cells_[(size_t)y * (size_t)width_ + (size_t)x];
}

bool NavGridHandle::isWalkable(int x, int y) const
{
	return validTile(x, y) && getMap(x, y) == 0;
}

int NavGridHandle::findStraightPath(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& paths)
{
	if(layer != 0 || width_ <= 0 || height_ <= 0)
		return NAV_ERROR;

	setMapLayer(layer);
	pCurrNavGridHandle = this;

	nodeStart = worldToGrid(start);
	nodeGoal = worldToGrid(end);

	if(!isWalkable(nodeStart.x, nodeStart.y) || !isWalkable(nodeGoal.x, nodeGoal.y))
		return NAV_ERROR;

	astarsearch.SetStartAndGoalStates(nodeStart, nodeGoal);

	unsigned int searchState = 0;
	do
	{
		searchState = astarsearch.SearchStep();
	}
	while(searchState == AStarSearch<MapSearchNode>::SEARCH_STATE_SEARCHING);

	int steps = 0;
	if(searchState == AStarSearch<MapSearchNode>::SEARCH_STATE_SUCCEEDED)
	{
		MapSearchNode* node = astarsearch.GetSolutionStart();
		for(;;)
		{
			node = astarsearch.GetSolutionNext();
			if(!node)
				break;

			paths.push_back(gridToWorld(node->x, node->y, start.y));
			steps++;
		}

		astarsearch.FreeSolutionNodes();
	}

	astarsearch.EnsureMemoryFreed();
	return steps > 0 ? steps : NAV_ERROR;
}

void NavGridHandle::bresenhamLine(const MapSearchNode& p0, const MapSearchNode& p1, std::vector<MapSearchNode>& results) const
{
	bresenhamLine(p0.x, p0.y, p1.x, p1.y, results);
}

void NavGridHandle::bresenhamLine(int x0, int y0, int x1, int y1, std::vector<MapSearchNode>& results) const
{
	bool steep = abs(y1 - y0) > abs(x1 - x0);
	if(steep)
	{
		swapInt(x0, y0);
		swapInt(x1, y1);
	}
	if(x0 > x1)
	{
		swapInt(x0, x1);
		swapInt(y0, y1);
	}

	int deltax = x1 - x0;
	int deltay = abs(y1 - y0);
	int error = 0;
	int ystep = y0 < y1 ? 1 : -1;
	int y = y0;

	for(int x = x0; x <= x1; ++x)
	{
		if(steep)
			results.push_back(MapSearchNode(y, x));
		else
			results.push_back(MapSearchNode(x, y));

		error += deltay;
		if(2 * error >= deltax)
		{
			y += ystep;
			error -= deltax;
		}
	}
}

int NavGridHandle::raycast(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec)
{
	if(layer != 0 || width_ <= 0 || height_ <= 0)
		return NAV_ERROR;

	MapSearchNode nodeStart = worldToGrid(start);
	MapSearchNode nodeEnd = worldToGrid(end);
	if(!isWalkable(nodeStart.x, nodeStart.y))
		return NAV_ERROR;

	std::vector<MapSearchNode> vec;
	bresenhamLine(nodeStart, nodeEnd, vec);

	if(!vec.empty())
		vec.erase(vec.begin());

	for(std::vector<MapSearchNode>::const_iterator iter = vec.begin(); iter != vec.end(); ++iter)
	{
		if(!isWalkable(iter->x, iter->y))
			break;

		hitPointVec.push_back(gridToWorld(iter->x, iter->y, start.y));
	}

	return hitPointVec.empty() ? NAV_ERROR : (int)hitPointVec.size();
}

int NavGridHandle::raycastNear(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec)
{
	return raycast(layer, start, end, hitPointVec);
}

int NavGridHandle::raycastAlong(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec)
{
	std::vector<Position3D> points;
	int result = raycast(layer, start, end, points);
	if(result <= 0)
	{
		MapSearchNode nodeStart = worldToGrid(start);
		if(isWalkable(nodeStart.x, nodeStart.y))
		{
			hitPointVec.push_back(start);
			return 1;
		}

		return result;
	}

	hitPointVec.push_back(points.back());
	return 1;
}

int NavGridHandle::GetHeight(const Position3D& start, float* h, int layer)
{
	if(layer != 0 || h == NULL)
		return NAV_ERROR;

	*h = start.y;
	return 1;
}

int NavGridHandle::Intersects(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec)
{
	return raycast(layer, start, end, hitPointVec);
}

bool NavGridHandle::findNearestWalkableTile(const MapSearchNode& start, MapSearchNode& result) const
{
	if(width_ <= 0 || height_ <= 0)
		return false;

	MapSearchNode clamped(
		std::max(0, std::min(start.x, width_ - 1)),
		std::max(0, std::min(start.y, height_ - 1)));

	if(isWalkable(clamped.x, clamped.y))
	{
		result = clamped;
		return true;
	}

	std::vector<uint8> visited((size_t)width_ * (size_t)height_, 0);
	std::queue<MapSearchNode> pending;
	pending.push(clamped);
	visited[(size_t)clamped.y * (size_t)width_ + (size_t)clamped.x] = 1;

	static const int offsets[8][2] =
	{
		{-1, 0}, {1, 0}, {0, -1}, {0, 1},
		{-1, -1}, {1, -1}, {-1, 1}, {1, 1}
	};

	while(!pending.empty())
	{
		MapSearchNode current = pending.front();
		pending.pop();

		for(size_t i = 0; i < 8; ++i)
		{
			const int nx = current.x + offsets[i][0];
			const int ny = current.y + offsets[i][1];
			if(!validTile(nx, ny))
				continue;

			const size_t index = (size_t)ny * (size_t)width_ + (size_t)nx;
			if(visited[index])
				continue;

			visited[index] = 1;
			if(isWalkable(nx, ny))
			{
				result = MapSearchNode(nx, ny);
				return true;
			}

			pending.push(MapSearchNode(nx, ny));
		}
	}

	return false;
}

int NavGridHandle::GetNearPos(const Position3D& start, Position3D& end, int layer)
{
	if(layer != 0 || width_ <= 0 || height_ <= 0)
		return NAV_ERROR;

	MapSearchNode nearest;
	if(!findNearestWalkableTile(worldToGrid(start), nearest))
		return NAV_ERROR;

	end = gridToWorld(nearest.x, nearest.y, start.y);
	return 1;
}

NavigationHandle* NavGridHandle::create(std::string name)
{
	if(name == "")
		return NULL;

	std::string path = "spaces/" + name + "/" + name + ".map";
	FILE* fp = Resmgr::getSingleton().openRes(path, "rb");
	if(!fp)
		return NULL;

	uint8 headerBytes[8];
	memset(headerBytes, 0, sizeof(headerBytes));
	if(fread(headerBytes, 1, sizeof(headerBytes), fp) != sizeof(headerBytes))
	{
		fclose(fp);
		return NULL;
	}

	const int32 width = readInt32LE(headerBytes);
	const int32 height = readInt32LE(headerBytes + 4);

	if(width <= 0 || height <= 0)
	{
		fclose(fp);
		return NULL;
	}

	if(fseek(fp, 0, SEEK_END) != 0)
	{
		fclose(fp);
		return NULL;
	}

	long flen = ftell(fp);
	if(flen < (long)sizeof(headerBytes))
	{
		fclose(fp);
		return NULL;
	}

	const size_t expectedSize = sizeof(headerBytes) + (size_t)width * (size_t)height;
	if((size_t)flen != expectedSize)
	{
		fclose(fp);
		return NULL;
	}

	if(fseek(fp, (long)sizeof(headerBytes), SEEK_SET) != 0)
	{
		fclose(fp);
		return NULL;
	}

	NavGridHandle* pNavGridHandle = new NavGridHandle();
	pNavGridHandle->width_ = width;
	pNavGridHandle->height_ = height;
	pNavGridHandle->cells_.resize((size_t)width * (size_t)height);

	if(!pNavGridHandle->cells_.empty())
	{
		if(fread(&pNavGridHandle->cells_[0], 1, pNavGridHandle->cells_.size(), fp) != pNavGridHandle->cells_.size())
		{
			fclose(fp);
			delete pNavGridHandle;
			return NULL;
		}
	}

	fclose(fp);
	pNavGridHandle->name = name;
	return pNavGridHandle;
}

bool NavGridHandle::MapSearchNode::IsSameState(MapSearchNode& rhs)
{
	return x == rhs.x && y == rhs.y;
}

void NavGridHandle::MapSearchNode::PrintNodeInfo()
{
	char str[100];
	sprintf(str, "NavGridHandle::MapSearchNode::printNodeInfo(): Node position : (%d,%d)\n", x, y);
	(void)str;
}

float NavGridHandle::MapSearchNode::GoalDistanceEstimate(MapSearchNode& nodeGoal)
{
	const float dx = fabsf((float)x - (float)nodeGoal.x);
	const float dy = fabsf((float)y - (float)nodeGoal.y);
	const float diag = std::min(dx, dy);
	const float straight = std::max(dx, dy) - diag;
	return diag * 1.41421356f + straight;
}

bool NavGridHandle::MapSearchNode::IsGoal(MapSearchNode& nodeGoal)
{
	return x == nodeGoal.x && y == nodeGoal.y;
}

bool NavGridHandle::MapSearchNode::GetSuccessors(AStarSearch<MapSearchNode>* astarsearch, MapSearchNode* parent_node)
{
	int parent_x = -1;
	int parent_y = -1;

	if(parent_node)
	{
		parent_x = parent_node->x;
		parent_y = parent_node->y;
	}

	static const int offsets[8][2] =
	{
		{-1, 0}, {0, -1}, {1, 0}, {0, 1},
		{1, 1}, {1, -1}, {-1, 1}, {-1, -1}
	};

	for(size_t i = 0; i < 8; ++i)
	{
		const int nx = x + offsets[i][0];
		const int ny = y + offsets[i][1];
		if(parent_x == nx && parent_y == ny)
			continue;

		if(!NavGridHandle::pCurrNavGridHandle->isWalkable(nx, ny))
			continue;

		if(offsets[i][0] != 0 && offsets[i][1] != 0)
		{
			if(!NavGridHandle::pCurrNavGridHandle->isWalkable(x + offsets[i][0], y) ||
				!NavGridHandle::pCurrNavGridHandle->isWalkable(x, y + offsets[i][1]))
			{
				continue;
			}
		}

		MapSearchNode newNode(nx, ny);
		astarsearch->AddSuccessor(newNode);
	}

	return true;
}

float NavGridHandle::MapSearchNode::GetCost(MapSearchNode& successor)
{
	return diagonalCost(x != successor.x && y != successor.y);
}

}
