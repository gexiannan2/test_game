// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "navigation_mesh_handle.h"

#include <algorithm>
#include <filesystem>
#include <memory>

namespace KBEngine{	

// Returns a random number [0..1)
static float frand()
{
//	return ((float)(rand() & 0xffff)/(float)0xffff);
	return (float)rand()/(float)RAND_MAX;
}

//-------------------------------------------------------------------------------------
NavMeshHandle::NavMeshHandle():
NavigationHandle(),
navmeshLayer()
{
}

//-------------------------------------------------------------------------------------
NavMeshHandle::~NavMeshHandle()
{
	std::map<int, NavmeshLayer>::iterator iter = navmeshLayer.begin();
	for(; iter != navmeshLayer.end(); ++iter)
	{
		dtFreeNavMesh(iter->second.pNavmesh);
		dtFreeNavMeshQuery(iter->second.pNavmeshQuery);
	}

	DEBUG_MSG(fmt::format("NavMeshHandle::~NavMeshHandle(): ({}) is destroyed!\n", resPath));
}

//-------------------------------------------------------------------------------------
void NavMeshHandle::clearObstacles()
{
	obstacles_.clear();
	nextObstacleRef_ = 1;
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::findStraightPath(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& paths)
{
	std::map<int, NavmeshLayer>::iterator iter = navmeshLayer.find(layer);
	if(iter == navmeshLayer.end())
	{
		ERROR_MSG(fmt::format("NavMeshHandle::findStraightPath: not found layer({})\n",  layer));
		return NAV_ERROR;
	}

	dtNavMeshQuery* navmeshQuery = iter->second.pNavmeshQuery;
	// dtNavMesh* 

	float spos[3];
	spos[0] = start.x;
	spos[1] = start.y;
	spos[2] = start.z;

	float epos[3];
	epos[0] = end.x;
	epos[1] = end.y;
	epos[2] = end.z;

	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);
	filter.setAreaCost(OBSTACLE_AREA_ID, 99999.f);  // 动态阻挡区域极高代价 → 寻路绕开

	const float extents[3] = {2.f, 4.f, 2.f};  

	dtPolyRef startRef = INVALID_NAVMESH_POLYREF;
	dtPolyRef endRef = INVALID_NAVMESH_POLYREF;

	float startNearestPt[3];
	float endNearestPt[3];
	navmeshQuery->findNearestPoly(spos, extents, &filter, &startRef, startNearestPt);
	navmeshQuery->findNearestPoly(epos, extents, &filter, &endRef, endNearestPt);

	if (!startRef || !endRef)
	{
		ERROR_MSG(fmt::format("NavMeshHandle::findStraightPath({2}): Could not find any nearby poly's ({0}, {1})\n", startRef, endRef, resPath));
		return NAV_ERROR_NEARESTPOLY;
	}

	dtPolyRef polys[MAX_POLYS];
	int npolys;
	float straightPath[MAX_POLYS * 3];
	unsigned char straightPathFlags[MAX_POLYS];
	dtPolyRef straightPathPolys[MAX_POLYS];
	int nstraightPath;
	int pos = 0;

	navmeshQuery->findPath(startRef, endRef, startNearestPt, endNearestPt, &filter, polys, &npolys, MAX_POLYS);
	nstraightPath = 0;

	if (npolys)
	{
		float epos1[3];
		dtVcopy(epos1, endNearestPt);
				
		if (polys[npolys-1] != endRef)
			navmeshQuery->closestPointOnPoly(polys[npolys-1], endNearestPt, epos1, 0);
				
		navmeshQuery->findStraightPath(startNearestPt, endNearestPt, polys, npolys, straightPath, straightPathFlags, straightPathPolys, &nstraightPath, MAX_POLYS);

		Position3D currpos;
		for(int i = 0; i < nstraightPath * 3; )
		{
			currpos.x = straightPath[i++];
			currpos.y = straightPath[i++];
			currpos.z = straightPath[i++];
			paths.push_back(currpos);
			pos++; 
			
			//DEBUG_MSG(fmt::format("NavMeshHandle::findStraightPath: {}->{}, {}, {}\n", pos, currpos.x, currpos.y, currpos.z));
		}
	}

	return pos;
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::findRandomPointAroundCircle(int layer, const Position3D& centerPos,
	std::vector<Position3D>& points, uint32 max_points, float maxRadius)
{
	std::map<int, NavmeshLayer>::iterator iter = navmeshLayer.find(layer);
	if (iter == navmeshLayer.end())
	{
		ERROR_MSG(fmt::format("NavMeshHandle::findRandomPointAroundCircle: not found layer({})\n", layer));
		return NAV_ERROR;
	}

	dtNavMeshQuery* navmeshQuery = iter->second.pNavmeshQuery;

	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);
	filter.setAreaCost(OBSTACLE_AREA_ID, 99999.f);  // 动态阻挡区域极高代价

	if (maxRadius <= 0.0001f)
	{
		Position3D currpos;

		for (uint32 i = 0; i < max_points; i++)
		{
			float pt[3];
			dtPolyRef ref;
			dtStatus status = navmeshQuery->findRandomPoint(&filter, frand, &ref, pt);

			if (dtStatusSucceed(status))
			{
				currpos.x = pt[0];
				currpos.y = pt[1];
				currpos.z = pt[2];

				points.push_back(currpos);
			}
		}

		return (int)points.size();
	}

	const float extents[3] = { 2.f, 4.f, 2.f };

	dtPolyRef startRef = INVALID_NAVMESH_POLYREF;

	float spos[3];
	spos[0] = centerPos.x;
	spos[1] = centerPos.y;
	spos[2] = centerPos.z;

	float startNearestPt[3];
	navmeshQuery->findNearestPoly(spos, extents, &filter, &startRef, startNearestPt);

	if (!startRef)
	{
		ERROR_MSG(fmt::format("NavMeshHandle::findRandomPointAroundCircle({1}): Could not find any nearby poly's ({0})\n", startRef, resPath));
		return NAV_ERROR_NEARESTPOLY;
	}

	const float squareSize = (float)maxRadius;
	float squareVerts[12] = {
		spos[0] - squareSize, spos[1], spos[2] + squareSize,\
		spos[0] + squareSize, spos[1], spos[2] + squareSize,\
		spos[0] + squareSize, spos[1], spos[2] - squareSize,\
		spos[0] - squareSize, spos[1], spos[2] - squareSize,\
	};

	static const int maxResult = 32;
	dtPolyRef polyRefs[maxResult];
	dtPolyRef parentPolyRefs[maxResult];
	int polyCount = 0;
	float cost[maxResult];

	navmeshQuery->findPolysAroundShape(startRef, squareVerts, 4, &filter, polyRefs, parentPolyRefs, cost, &polyCount, maxResult);

	if (polyCount == 0)
	{
		return (int)points.size();
	}

	float* allPolyAreas = new float[polyCount];
	Position3D currpos;
	for (uint32 time = 0; time < max_points; time++)
	{
		const dtMeshTile* randomTile = 0;
		const dtPoly* randomPoly = 0;
		dtPolyRef randomPolyRef = 0;
		float areaSum = 0.0f;

		for (int i = 0; i < polyCount; i++)
		{
			float polyArea = 0.0f;

			if (time == 0)
			{
				const dtMeshTile* tile = 0;
				const dtPoly* poly = 0;
				dtPolyRef ref = polyRefs[i];
				navmeshQuery->getAttachedNavMesh()->getTileAndPolyByRefUnsafe(ref, &tile, &poly);

				if (poly->getType() != DT_POLYTYPE_GROUND) continue;

				// Place random locations on on ground.
				if (poly->getType() == DT_POLYTYPE_GROUND)
				{
					// Calc area of the polygon.
					for (int j = 2; j < poly->vertCount; ++j)
					{
						const float* va = &tile->verts[poly->verts[0] * 3];
						const float* vb = &tile->verts[poly->verts[j - 1] * 3];
						const float* vc = &tile->verts[poly->verts[j] * 3];
						polyArea += dtTriArea2D(va, vb, vc);
					}

					allPolyAreas[i] = polyArea;
					areaSum += polyArea;
					const float u = frand();

					if (u*areaSum <= polyArea)
					{
						randomTile = tile;
						randomPoly = poly;
						randomPolyRef = ref;
					}
				}
			}
			else
			{
				// Choose random polygon weighted by area, using reservoi sampling.
				areaSum += allPolyAreas[i];
				const float u = frand();

				if (u*areaSum <= allPolyAreas[i])
				{
					const dtMeshTile* tile = 0;
					const dtPoly* poly = 0;
					dtPolyRef ref = polyRefs[i];
					navmeshQuery->getAttachedNavMesh()->getTileAndPolyByRefUnsafe(ref, &tile, &poly);

					if (poly->getType() != DT_POLYTYPE_GROUND) continue;

					randomTile = tile;
					randomPoly = poly;
					randomPolyRef = ref;
				}
			}
		}

		// Randomly pick point on polygon.
		dtPolyRef randomRef = INVALID_NAVMESH_POLYREF;
		const float* v = &randomTile->verts[randomPoly->verts[0] * 3];
		float verts[3 * DT_VERTS_PER_POLYGON];
		float areas[DT_VERTS_PER_POLYGON + 4];
		dtVcopy(&verts[0 * 3], v);

		for (int j = 1; j < randomPoly->vertCount; ++j)
		{
			v = &randomTile->verts[randomPoly->verts[j] * 3];
			dtVcopy(&verts[j * 3], v);
		}

		float overlapPolyVerts[(DT_VERTS_PER_POLYGON + 4) * 3];
		int nOverlapPolyVerts = 0;

		getOverlapPolyPoly2D(squareVerts, 4, verts, randomPoly->vertCount, overlapPolyVerts, &nOverlapPolyVerts);

		if (nOverlapPolyVerts <= 0)
		{
			delete[] allPolyAreas;
			return (int)points.size();
		}

		const float s = frand();
		const float t = frand();
		float pt[3];
		dtRandomPointInConvexPoly(overlapPolyVerts, nOverlapPolyVerts, areas, s, t, pt);

		float h = 0.0f;
		dtStatus status = navmeshQuery->getPolyHeight(randomPolyRef, pt, &h);

		if (dtStatusFailed(status))
		{
			delete[] allPolyAreas;
			return (int)points.size();
		}

		pt[1] = h;
		randomRef = randomPolyRef;

		if (randomRef)
		{
			currpos.x = pt[0];
			currpos.y = pt[1];
			currpos.z = pt[2];

			float src_len = sqrt(2) * squareSize;
			float xx = centerPos.x - currpos.x;
			float yy = centerPos.y - currpos.y;
			float dist_len = sqrt(xx * xx + yy * yy);

			if (dist_len > src_len)
			{
				ERROR_MSG(fmt::format("NavMeshHandle::findRandomPointAroundCircle::(Out of range)::centerPos({},{},{}), currpos({},{},{}), errLen({}), {}, {}\n", 
					centerPos.x, centerPos.y, centerPos.z, currpos.x, currpos.y, currpos.z, (dist_len - src_len), dist_len, src_len));

				continue;
			}

			points.push_back(currpos);
		}
	}

	delete[] allPolyAreas;

	return (int)points.size();
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::raycast(int layer, const Position3D& start, const Position3D& end, std::vector<Position3D>& hitPointVec)
{
	std::map<int, NavmeshLayer>::iterator iter = navmeshLayer.find(layer);
	if(iter == navmeshLayer.end())
	{
		ERROR_MSG(fmt::format("NavMeshHandle::raycast: not found layer({})\n",  layer));
		return NAV_ERROR;
	}

	dtNavMeshQuery* navmeshQuery = iter->second.pNavmeshQuery;

	float hitPoint[3];

	float spos[3];
	spos[0] = start.x;
	spos[1] = start.y;
	spos[2] = start.z;

	float epos[3];
	epos[0] = end.x;
	epos[1] = end.y;
	epos[2] = end.z;

	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);
	filter.setAreaCost(OBSTACLE_AREA_ID, 99999.f);  // 动态阻挡区域极高代价（影响寻路代价，不影响 raycast）

	const float extents[3] = {2.f, 4.f, 2.f};

	dtPolyRef startRef = INVALID_NAVMESH_POLYREF;

	float nearestPt[3];
	navmeshQuery->findNearestPoly(spos, extents, &filter, &startRef, nearestPt);

	if (!startRef)
	{
		return NAV_ERROR_NEARESTPOLY;
	}

	float t = 0;
	float hitNormal[3];
	memset(hitNormal, 0, sizeof(hitNormal));

	dtPolyRef polys[MAX_POLYS];
	int npolys;

	navmeshQuery->raycast(startRef, spos, epos, &filter, &t, hitNormal, polys, &npolys, MAX_POLYS);

	if (t > 1)
	{
		// ── Detour raycast 报告无碰撞 ──
		// 但 Detour raycast 存在盲区：当射线终点落在当前 poly 内部（segMax==-1）时，
		// 它不检查该 poly 的 flags/area，导致动态阻挡被漏检。
		// 此处通过射线与障碍物 AABB 相交检测来补漏。
		float rayDir[3] = { epos[0] - spos[0], epos[1] - spos[1], epos[2] - spos[2] };

		for (const auto& obs : obstacles_)
		{
			if (rayIntersectsAABB(spos, rayDir, obs.boundMin, obs.boundMax))
			{
				// 射线穿过障碍物包围盒 → 报告碰撞，命中点取障碍物中心
				hitPointVec.push_back(Position3D(
					(obs.boundMin[0] + obs.boundMax[0]) * 0.5f,
					(obs.boundMin[1] + obs.boundMax[1]) * 0.5f,
					(obs.boundMin[2] + obs.boundMax[2]) * 0.5f));
				return 1;
			}
		}

		// 确实无碰撞
		return NAV_ERROR;
	}
	else
	{
		// Hit
		hitPoint[0] = spos[0] + (epos[0] - spos[0]) * t;
		hitPoint[1] = spos[1] + (epos[1] - spos[1]) * t;
		hitPoint[2] = spos[2] + (epos[2] - spos[2]) * t;
		if (npolys)
		{
			float h = 0;
			navmeshQuery->getPolyHeight(polys[npolys-1], hitPoint, &h);
			hitPoint[1] = h;
		}
	}
	
	hitPointVec.push_back(Position3D(hitPoint[0], hitPoint[1], hitPoint[2]));
	return 1;
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::GetNearPos(const Position3D& start, Position3D& end, int layer)
{
	std::map<int, NavmeshLayer>::iterator iter = navmeshLayer.find(layer);
	if (iter == navmeshLayer.end())
	{
		ERROR_MSG(fmt::format("NavMeshHandle::GetNearPos: not found layer({})\n", layer));
		return NAV_ERROR;
	}

	dtNavMeshQuery* navmeshQuery = iter->second.pNavmeshQuery;

	float spos[3];
	spos[0] = start.x;
	spos[1] = start.y;
	spos[2] = start.z;

	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);

	const float extents[3] = {20.f, 50.f, 20.f};  

	dtPolyRef nearRef = INVALID_NAVMESH_POLYREF;
	float nearPt[3];
	navmeshQuery->findNearestPoly(spos, extents, &filter, &nearRef, nearPt);

	if (!nearRef)
	{
		return NAV_ERROR_NEARESTPOLY;
	}

	end.x = nearPt[0];
	end.y = nearPt[1];
	end.z = nearPt[2];
	return 0;
}

// 用于 GetHeight：遍历 XZ 区域内所有 polygon，取最高高度（地表层）
namespace
{
	struct HeightQuery : public dtPolyQuery
	{
		dtNavMeshQuery* navQuery;
		const float* pos;
		float bestHeight;
		bool found;
		dtPolyRef bestRef;

		HeightQuery(dtNavMeshQuery* q, const float* p) : navQuery(q), pos(p), bestHeight(-FLT_MAX), found(false), bestRef(0) {}

		void process(const dtMeshTile* /*tile*/, dtPoly** /*polys*/, dtPolyRef* refs, int count) override
		{
			for (int i = 0; i < count; ++i)
			{
				float h;
				if (dtStatusSucceed(navQuery->getPolyHeight(refs[i], pos, &h)) && h > bestHeight)
				{
					bestHeight = h;
					found = true;
					bestRef = refs[i];
				}
			}
		}
	};
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::GetHeight(int layer, const Position3D& pos, float& height)
{
	std::map<int, NavmeshLayer>::iterator iter = navmeshLayer.find(layer);
	if (iter == navmeshLayer.end())
	{
		ERROR_MSG(fmt::format("NavMeshHandle::GetHeight: not found layer({})\n", layer));
		return NAV_ERROR;
	}

	dtNavMeshQuery* navmeshQuery = iter->second.pNavmeshQuery;
	dtNavMesh* navmesh = iter->second.pNavmesh;   // 用于直接读 tile detail 数据

	float spos[3];
	spos[0] = pos.x;
	spos[1] = pos.y;
	spos[2] = pos.z;

	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);

	// XZ 窄范围匹配，Y 全范围覆盖多层地形（高台/地面/地下）
	const float extents[3] = {2.f, 500.f, 2.f};

	// 遍历 XZ 区域内所有 polygon，取最高高度（地表层优先）
	HeightQuery query(navmeshQuery, spos);
	navmeshQuery->queryPolygons(spos, extents, &filter, &query);

	if (!query.found)
	{
		// 回退：XZ 区域内无 polygon，尝试 findNearestPoly
		dtPolyRef nearRef = INVALID_NAVMESH_POLYREF;
		float nearPt[3];
		navmeshQuery->findNearestPoly(spos, extents, &filter, &nearRef, nearPt);
		if (!nearRef)
			return NAV_ERROR_NEARESTPOLY;
		navmeshQuery->getPolyHeight(nearRef, spos, &height);
		return 0;
	}

	height = query.bestHeight;

	// 定位具体是哪个 detail triangle 算出了这个高度
	if (navmesh && query.bestRef)
	{
		const dtMeshTile* tile = nullptr;
		const dtPoly* poly = nullptr;
		if (dtStatusSucceed(navmesh->getTileAndPolyByRef(query.bestRef, &tile, &poly)) && tile && poly)
		{
			unsigned int ip = (unsigned int)(poly - tile->polys);
			if (ip < (unsigned int)tile->header->detailMeshCount && tile->detailMeshes && tile->detailTris && tile->detailVerts)
			{
				const dtPolyDetail* pd = &tile->detailMeshes[ip];
				for (int j = 0; j < pd->triCount; ++j)
				{
					const unsigned char* t = &tile->detailTris[(pd->triBase + j) * 4];
					const float* v[3] = { nullptr, nullptr, nullptr };
					bool valid = true;
					for (int k = 0; k < 3; ++k)
					{
						if (t[k] < poly->vertCount)
						{
							if (poly->verts[t[k]] < tile->header->vertCount)
								v[k] = &tile->verts[poly->verts[t[k]] * 3];
							else
								valid = false;
						}
						else
						{
							unsigned int dvIdx = pd->vertBase + (t[k] - poly->vertCount);
							if (dvIdx < (unsigned int)tile->header->detailVertCount)
								v[k] = &tile->detailVerts[dvIdx * 3];
							else
								valid = false;
						}
					}
					if (!valid || !v[0] || !v[1] || !v[2])
					{
						continue;
					}

					// 直接复用 Detour 的 XZ 包含测试，保证和 getPolyHeight 一致
					float h;
					if (dtClosestHeightPointTriangle(spos, v[0], v[1], v[2], h))
					{
						char buf[256];
						snprintf(buf, sizeof(buf),
							"[GetHeight] poly=%d ref=%d tri=%d/%d q=(%.2f,%.2f,%.2f) h=%.4f",
							(int)ip, (int)query.bestRef, j, (int)pd->triCount,
							spos[0], spos[1], spos[2], h);
						DEBUG_MSG(fmt::format("{}\n", buf));
						break;
					}
				}
			}
		}
	}

	return 0;
}

//-------------------------------------------------------------------------------------
NavigationHandle* NavMeshHandle::create(std::string resPath, const std::map< int, std::string >& params)
{
	if(resPath.empty())
		return NULL;

	NavMeshHandle* pNavMeshHandle = NULL;
	const std::filesystem::path base_path(resPath);

	if(params.size() == 0)
	{
		std::error_code error;
		std::vector<std::filesystem::path> results;
		for (std::filesystem::directory_iterator iterator(base_path, error), end;
			 !error && iterator != end;
			 iterator.increment(error))
		{
			if (iterator->is_regular_file(error) &&
				iterator->path().extension() == ".navmesh")
			{
				results.push_back(iterator->path());
			}
		}
		std::sort(results.begin(), results.end());

		if(error || results.empty())
		{
			ERROR_MSG(fmt::format(
				"NavMeshHandle::create: path({}) contains no navmesh files.\n",
				resPath));
			return NULL;
		}

		pNavMeshHandle = new NavMeshHandle();
		int layer = 0;
		for(const auto& result : results)
		{
			_create(layer++, resPath, result.string(), pNavMeshHandle);
		}
	}
	else
	{
		pNavMeshHandle = new NavMeshHandle();
		std::map< int, std::string >::const_iterator iter = params.begin();

		for(; iter != params.end(); ++iter)
		{
			const std::filesystem::path configured_path(iter->second);
			const std::filesystem::path fullpath = configured_path.is_absolute()
				? configured_path
				: base_path / configured_path;
			DEBUG_MSG(fmt::format("NavMeshHandle::create: try open({}) layer={}\n",
				fullpath.string(), iter->first));
			_create(iter->first, resPath, fullpath.string(), pNavMeshHandle);
		}		
	}
	
	// 检查是否成功加载了任何层数据，防止返回空壳句柄导致后续误判
	if (pNavMeshHandle && pNavMeshHandle->navmeshLayer.empty())
	{
		ERROR_MSG(fmt::format("NavMeshHandle::create: ({}) no layer loaded!\n", resPath));
		delete pNavMeshHandle;
		return NULL;
	}
	
	return pNavMeshHandle;
}

//-------------------------------------------------------------------------------------
template<typename NAVMESH_SET_HEADER>
dtNavMesh* tryReadNavmesh(uint8* data, size_t readsize, const std::string& res, bool showlog)
{
	if (readsize < sizeof(NAVMESH_SET_HEADER))
	{
		if(showlog)
		{
			ERROR_MSG(fmt::format(
				"NavMeshHandle::tryReadNavmesh: open({}), NavMeshSetHeader error!\n",
				res));
		}

		return NULL;
	}
	
	int pos = 0;
	int size = 0;
	bool safeStorage = true;
	
	NAVMESH_SET_HEADER header;
	size = sizeof(NAVMESH_SET_HEADER);
	
	memcpy(&header, data, size);

	if (header.version != NavMeshHandle::RCN_NAVMESH_VERSION ||
		header.tileCount < 0)
	{
		if(showlog)
		{
			ERROR_MSG(fmt::format("NavMeshHandle::tryReadNavmesh: navmesh version({}) is not match({})!\n", 
				header.version, ((int)NavMeshHandle::RCN_NAVMESH_VERSION)));
		}
		
		return NULL;
	}

	dtNavMesh* mesh = dtAllocNavMesh();
	if (!mesh)
	{
		if(showlog)
		{
			ERROR_MSG("NavMeshHandle::tryReadNavmesh: dtAllocNavMesh is failed!\n");
		}
		
		return NULL;
	}

	dtStatus status = mesh->init(&header.params);
	if (dtStatusFailed(status))
	{
		if(showlog)
		{
			ERROR_MSG(fmt::format("NavMeshHandle::tryReadNavmesh: mesh init error({})!\n", status));
		}
		
		dtFreeNavMesh(mesh);
		return NULL;
	}

	// Read tiles.
	bool success = true;
	pos += size;

	for (int i = 0; i < header.tileCount; ++i)
	{
		if (static_cast<size_t>(pos) + sizeof(NavMeshTileHeader) > readsize)
		{
			success = false;
			status = DT_FAILURE | DT_INVALID_PARAM;
			break;
		}

		NavMeshTileHeader tileHeader;
		size = sizeof(NavMeshTileHeader);

		memcpy(&tileHeader, &data[pos], size);
		pos += size;

		size = tileHeader.dataSize;
		if (!tileHeader.tileRef ||
			tileHeader.dataSize <= 0 ||
			static_cast<size_t>(pos) + static_cast<size_t>(tileHeader.dataSize) > readsize)
		{
			success = false;
			status = DT_FAILURE | DT_INVALID_PARAM;
			break;
		}
		
		unsigned char* tileData = 
			(unsigned char*)dtAlloc(size, DT_ALLOC_PERM);

		if (!tileData)
		{
			success = false;
			status = DT_FAILURE + DT_OUT_OF_MEMORY;
			break;
		}
		memcpy(tileData, &data[pos], size);
		pos += size;

		status = mesh->addTile(tileData
			, size
			, (safeStorage ? DT_TILE_FREE_DATA : 0)
			, tileHeader.tileRef
			, 0);

		if (dtStatusFailed(status))
		{
			dtFree(tileData);
			success = false;
			break;
		}
	}
	
	if (!success)
	{
		if(showlog)
		{
			ERROR_MSG(fmt::format("NavMeshHandle::tryReadNavmesh:  error({})!\n", status));
		}
		
		dtFreeNavMesh(mesh);
		return NULL;
	}
	
	return mesh;
}

bool NavMeshHandle::_create(int layer, const std::string& resPath, const std::string& res, NavMeshHandle* pNavMeshHandle)
{
	KBE_ASSERT(pNavMeshHandle);
	std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(res.c_str(), "rb"), &fclose);
	if (!fp)
	{
		ERROR_MSG(fmt::format("NavMeshHandle::_create: fopen failed path=({})\n", res));
		return false;
	}

	DEBUG_MSG(fmt::format("NavMeshHandle::_create: ({}), layer={}\n", 
		res, layer));

	if (fseek(fp.get(), 0, SEEK_END) != 0)
	{
		ERROR_MSG(fmt::format("NavMeshHandle::create: seek failed for ({}).\n", res));
		return false;
	}

	const long file_length = ftell(fp.get());
	if (file_length <= 0 || fseek(fp.get(), 0, SEEK_SET) != 0)
	{
		ERROR_MSG(fmt::format("NavMeshHandle::create: invalid file size for ({}).\n", res));
		return false;
	}

	const size_t flen = static_cast<size_t>(file_length);
	std::vector<uint8> data(flen);
	const size_t readsize = fread(data.data(), 1, flen, fp.get());
	if (readsize != flen)
	{
		ERROR_MSG(fmt::format(
			"NavMeshHandle::create: open({}), read(size={} != {}) error!\n",
			res, readsize, flen));
		return false;
	}

	dtNavMesh* mesh = tryReadNavmesh<NavMeshSetHeader>(
		data.data(), readsize, res, false);
	
	// 如果加载失败则尝试加载扩展格式
	if(!mesh)
		mesh = tryReadNavmesh<NavMeshSetHeaderEx>(
			data.data(), readsize, res, true);

	if (!mesh)
	{
		ERROR_MSG("NavMeshHandle::create: dtAllocNavMesh is failed!\n");
		return false;
	}

	dtNavMeshQuery* pMavmeshQuery = dtAllocNavMeshQuery();
	if (!pMavmeshQuery)
	{
		dtFreeNavMesh(mesh);
		ERROR_MSG("NavMeshHandle::create: dtAllocNavMeshQuery failed!\n");
		return false;
	}
	const dtStatus query_status = pMavmeshQuery->init(mesh, 2048);
	if (dtStatusFailed(query_status))
	{
		dtFreeNavMeshQuery(pMavmeshQuery);
		dtFreeNavMesh(mesh);
		ERROR_MSG(fmt::format(
			"NavMeshHandle::create: query init failed({}).\n",
			query_status));
		return false;
	}
	pNavMeshHandle->resPath = resPath;
	pNavMeshHandle->navmeshLayer[layer].pNavmeshQuery = pMavmeshQuery;
	pNavMeshHandle->navmeshLayer[layer].pNavmesh = mesh;
	
	uint32 tileCount = 0;
	uint32 nodeCount = 0;
	uint32 polyCount = 0;
	uint32 vertCount = 0;
	uint32 triCount = 0;
	uint32 triVertCount = 0;
	uint32 dataSize = 0;

	const dtNavMesh* navmesh = mesh;
	for (int32 i = 0; i < navmesh->getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navmesh->getTile(i);
		if (!tile || !tile->header)
			continue;

		tileCount ++;
		nodeCount += tile->header->bvNodeCount;
		polyCount += tile->header->polyCount;
		vertCount += tile->header->vertCount;
		triCount += tile->header->detailTriCount;
		triVertCount += tile->header->detailVertCount;
		dataSize += tile->dataSize;

		// DEBUG_MSG(fmt::format("NavMeshHandle::create: verts({}, {}, {})\n", tile->verts[0], tile->verts[1], tile->verts[2]));
	}

	DEBUG_MSG(fmt::format("\t==> tiles loaded: {}\n", tileCount));
	DEBUG_MSG(fmt::format("\t==> BVTree nodes: {}\n", nodeCount));
	DEBUG_MSG(fmt::format("\t==> {} polygons ({} vertices)\n", polyCount, vertCount));
	DEBUG_MSG(fmt::format("\t==> {} triangles ({} vertices)\n", triCount, triVertCount));
	DEBUG_MSG(fmt::format("\t==> {:.2f} MB of data (not including pointers)\n", (((float)dataSize / sizeof(unsigned char)) / 1048576)));
	
	return true;
}

//-------------------------------------------------------------------------------------
// 动态阻挡实现（setPolyArea 方案）
//-------------------------------------------------------------------------------------
int NavMeshHandle::MarkPolygons(int layer, const float* bmin, const float* bmax,
	std::vector<dtPolyRef>& outPolys, std::vector<unsigned char>& outOrigAreas,
	std::vector<unsigned short>* outOrigFlags)
{
	auto it = navmeshLayer.find(layer);
	if (it == navmeshLayer.end())
	{
		return -1;
	}

	dtNavMeshQuery* query = it->second.pNavmeshQuery;
	dtNavMesh*       mesh  = it->second.pNavmesh;

	// 计算查询中心 + 半边长（+1.f 容差避免边界 poly 遗漏）
	float center[3] = {
		(bmin[0] + bmax[0]) * 0.5f,
		(bmin[1] + bmax[1]) * 0.5f,
		(bmin[2] + bmax[2]) * 0.5f
	};
	float halfExt[3] = {
		(bmax[0] - bmin[0]) * 0.5f + 1.f,
		(bmax[1] - bmin[1]) * 0.5f + 1.f,
		(bmax[2] - bmin[2]) * 0.5f + 1.f
	};

	dtQueryFilter filter;
	filter.setIncludeFlags(0xffff);
	filter.setExcludeFlags(0);

	static const int QUERY_MAX = 512;
	dtPolyRef polys[QUERY_MAX];
	int polyCount = 0;

	dtStatus st = query->queryPolygons(center, halfExt, &filter,
		polys, &polyCount, QUERY_MAX);
	if (dtStatusFailed(st))
	{
		return -2;
	}

	for (int i = 0; i < polyCount; ++i)
	{
		unsigned char origArea = 0;
		mesh->getPolyArea(polys[i], &origArea);
		if (origArea == OBSTACLE_AREA_ID)
		{
			continue;  // 已被其他障碍物占用，跳过以避免重复记录
		}

		unsigned short origFlag = 0;
		mesh->getPolyFlags(polys[i], &origFlag);

		mesh->setPolyArea(polys[i], OBSTACLE_AREA_ID);
		// 关键修复：清除 flags 使寻路 passFilter 排除此多边形
		// (dtQueryFilter::passFilter 检查 flags，不检查 area)
		mesh->setPolyFlags(polys[i], 0);

		outPolys.push_back(polys[i]);
		outOrigAreas.push_back(origArea);

		if (outOrigFlags)
		{
			outOrigFlags->push_back(origFlag);
		}
	}

	return outPolys.empty() ? -3 : 0;
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::AddObstacle(int layer, const float* center,
	float radius, float height, std::uint32_t& outRef)
{
	float bmin[3] = {center[0] - radius, center[1],          center[2] - radius};
	float bmax[3] = {center[0] + radius, center[1] + height, center[2] + radius};
	return AddBoxObstacle(layer, bmin, bmax, outRef);
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::AddBoxObstacle(int layer, const float* bmin, const float* bmax,
	std::uint32_t& outRef)
{
	if (static_cast<int>(obstacles_.size()) >= kMaxObstacles)
	{
		return -4;  // 超出上限
	}

	ObstacleRecord rec;
	rec.ref = nextObstacleRef_++;
	std::memcpy(rec.boundMin, bmin, sizeof(float) * 3);
	std::memcpy(rec.boundMax, bmax, sizeof(float) * 3);

	std::vector<unsigned short> origFlags;
	int ret = MarkPolygons(layer, bmin, bmax, rec.polys, rec.origAreas, &origFlags);
	if (ret < 0)
	{
		return ret;
	}

	rec.origFlags.swap(origFlags);

	obstacles_.push_back(std::move(rec));
	outRef = rec.ref;
	return 0;
}

//-------------------------------------------------------------------------------------
int NavMeshHandle::RemoveObstacle(int layer, std::uint32_t ref)
{
	auto it = navmeshLayer.find(layer);
	if (it == navmeshLayer.end())
	{
		return -1;
	}

	dtNavMesh* mesh = it->second.pNavmesh;

	for (auto obs = obstacles_.begin(); obs != obstacles_.end(); ++obs)
	{
		if (obs->ref != ref)
		{
			continue;
		}

		for (size_t i = 0; i < obs->polys.size(); ++i)
		{
			mesh->setPolyArea(obs->polys[i], obs->origAreas[i]);
			if (i < obs->origFlags.size())
			{
				mesh->setPolyFlags(obs->polys[i], obs->origFlags[i]);
			}
			else
			{
				// 兼容旧数据（删除前未记录 flags 的情况，恢复默认 walkable flags=1）
				mesh->setPolyFlags(obs->polys[i], 1);
			}
		}

		obstacles_.erase(obs);
		return 0;
	}

	return -2;  // 未找到该 ref
}

//-------------------------------------------------------------------------------------
bool NavMeshHandle::rayIntersectsAABB(const float* origin, const float* dir,
	const float* aabbMin, const float* aabbMax) const
{
	// Slab 算法（Kay/Kajiya 1986）：射线与 AABB 相交检测
	// 对每个轴计算进入/离开参数 t，检查是否有有效重叠区间
	float tmin = 0.f;
	float tmax = 1.f;  // 只检测 [0,1] 区间内的线段

	for (int i = 0; i < 3; ++i)
	{
		if (fabsf(dir[i]) < 1e-6f)
		{
			// 射线方向几乎垂直于该轴 → 检查起点是否在 AABB 范围内
			if (origin[i] < aabbMin[i] || origin[i] > aabbMax[i])
			{
				return false;
			}
		}
		else
		{
			const float ood = 1.f / dir[i];
			float t1 = (aabbMin[i] - origin[i]) * ood;
			float t2 = (aabbMax[i] - origin[i]) * ood;
			if (t1 > t2)
			{
				float tmp = t1;
				t1 = t2;
				t2 = tmp;
			}
			if (t1 > tmin)
			{
				tmin = t1;
			}
			if (t2 < tmax)
			{
				tmax = t2;
			}
			if (tmin > tmax)
			{
				return false;  // 无重叠
			}
		}
	}

	// 线段 [0,1] 与 AABB 有交集
	return true;
}

//-------------------------------------------------------------------------------------
inline float calAtan(float* srcPoint, float* point)
{
	return atan2(point[2] - srcPoint[2], point[0] - srcPoint[0]);
}

inline void swapPoint(float* a, float* b)
{
	float tmp[3] = { a[0],a[1],a[2] };
	a[0] = b[0];
	a[1] = b[1];
	a[2] = b[2];
	b[0] = tmp[0];
	b[1] = tmp[1];
	b[2] = tmp[2];
}

void NavMeshHandle::getOverlapPolyPoly2D(const float* polyVertsA, const int nPolyVertsA, const float* polyVertsB, const int nPolyVertsB, float* intsectPt, int* intsectPtCount)
{
	*intsectPtCount = 0;

	///Find polyA's verts which in polyB.
	for (int i = 0; i < nPolyVertsA; ++i)
	{
		const float* va = &polyVertsA[i * 3];
		if (dtPointInPolygon(va, polyVertsB, nPolyVertsB))
		{
			intsectPt[*intsectPtCount * 3] = va[0];
			intsectPt[*intsectPtCount * 3 + 1] = va[1];
			intsectPt[*intsectPtCount * 3 + 2] = va[2];
			*intsectPtCount += 1;
		}
	}

	///Find polyB's verts which in polyA.
	for (int i = 0; i < nPolyVertsB; ++i)
	{
		const float* va = &polyVertsB[i * 3];
		if (dtPointInPolygon(va, polyVertsA, nPolyVertsA))
		{
			intsectPt[*intsectPtCount * 3] = va[0];
			intsectPt[*intsectPtCount * 3 + 1] = va[1];
			intsectPt[*intsectPtCount * 3 + 2] = va[2];
			*intsectPtCount += 1;
		}
	}

	///Find edge intersection of polyA and polyB.
	for (int i = 0; i < nPolyVertsA; ++i)
	{
		const float* p1 = &polyVertsA[i * 3];
		int p2_idx = (i + 1) % nPolyVertsA;
		const float* p2 = &polyVertsA[p2_idx * 3];

		for (int j = 0; j < nPolyVertsB; ++j)
		{
			const float* q1 = &polyVertsB[j * 3];
			int q2_idx = (j + 1) % nPolyVertsB;
			const float* q2 = &polyVertsB[q2_idx * 3];

			if (isSegSegCross2D(p1, p2, q1, q2))				 ///If two segment is cross
			{
				float s, t;
				if (dtIntersectSegSeg2D(p1, p2, q1, q2, s, t))	///Caculate intersection point
				{
					float pt[3];
					dtVlerp(pt, q1, q2, t);
					intsectPt[*intsectPtCount * 3] = pt[0];
					intsectPt[*intsectPtCount * 3 + 1] = pt[1];
					intsectPt[*intsectPtCount * 3 + 2] = pt[2];
					*intsectPtCount += 1;
				}
			}
		}
	}

	///sort intersection to clockwise.
	if (*intsectPtCount > 0)
	{
		clockwiseSortPoints(intsectPt, *intsectPtCount);
	}

}

void NavMeshHandle::clockwiseSortPoints(float* verts, const int nVerts)
{
	float x = 0.0;
	float z = 0.0;
	for (int i = 0; i < nVerts; ++i)
	{
		x += verts[i * 3];
		z += verts[i * 3 + 2];
	}

	//Put most left point in first position.
	for (int i = 0; i < nVerts; i++)
	{
		if (verts[i * 3] < verts[0])
		{
			swapPoint(&verts[i * 3], &verts[0]);
		}
		else if (verts[i * 3] == verts[0] && verts[i * 3 + 2] < verts[2])
		{
			swapPoint(&verts[i * 3], &verts[0]);
		}
	}

	//Sort points by slope.
	for (int i = 1; i < nVerts; i++)
	{
		for (int j = 1; j < nVerts - i; j++)
		{
			int index = j * 3;
			int n_index = (j + 1) * 3;
			float angle = calAtan(&verts[0], &verts[index]);
			float n_angle = calAtan(&verts[0], &verts[n_index]);
			if (angle < n_angle)
			{
				swapPoint(&verts[index], &verts[n_index]);
			}
		}
	}
}

bool NavMeshHandle::isSegSegCross2D(const float* p1, const float *p2, const float* q1, const float* q2)
{
	bool ret = dtMin(p1[0], p2[0]) <= dtMax(q1[0], q2[0]) &&
		dtMin(q1[0], q2[0]) <= dtMax(p1[0], p2[0]) &&
		dtMin(p1[2], p2[2]) <= dtMax(q1[2], q2[2]) &&
		dtMin(q1[2], q2[2]) <= dtMax(p1[2], p2[2]);

	if (!ret)
	{
		return false;
	}

	long line1, line2;
	line1 = (long)(p1[0] * (q1[2] - p2[2]) + p2[0] * (p1[2] - q1[2]) + q1[0] * (p2[2] - p1[2]));
	line2 = (long)(p1[0] * (q2[2] - p2[2]) + p2[0] * (p1[2] - q2[2]) + q2[0] * (p2[2] - p1[2]));
	if (((line1 ^ line2) >= 0) && !(line1 == 0 && line2 == 0))
	{
		return false;
	}

	line1 = (long)(q1[0] * (p1[2] - q2[2]) + q2[0] * (q1[2] - p1[2]) + p1[0] * (q2[2] - q1[2]));
	line2 = (long)(q1[0] * (p2[2] - q2[2]) + q2[0] * (q1[2] - p2[2]) + p2[0] * (q2[2] - q1[2]));
	if (((line1 ^ line2) >= 0) && !(line1 == 0 && line2 == 0))
	{
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------

}





