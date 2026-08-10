#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

namespace
{

struct NavMeshSetHeader
{
	int version;
	int tileCount;
	dtNavMeshParams params;
};

struct NavMeshTileHeader
{
	dtTileRef tileRef;
	int dataSize;
};

struct LoadedNavMesh
{
	dtNavMesh* navMesh = 0;
	dtNavMeshQuery* navQuery = 0;

	~LoadedNavMesh()
	{
		if (navQuery)
		{
			dtFreeNavMeshQuery(navQuery);
			navQuery = 0;
		}
		if (navMesh)
		{
			dtFreeNavMesh(navMesh);
			navMesh = 0;
		}
	}
};

struct PolyPoint
{
	dtPolyRef ref = 0;
	float pos[3] = {0.f, 0.f, 0.f};
};

template <typename T>
T readStruct(const unsigned char* data, size_t size, size_t offset, const char* what)
{
	if (offset + sizeof(T) > size)
	{
		std::ostringstream oss;
		oss << "Unexpected EOF while reading " << what << " at offset " << offset;
		throw std::runtime_error(oss.str());
	}

	T value;
	std::memcpy(&value, data + offset, sizeof(T));
	return value;
}

std::vector<unsigned char> readFile(const std::string& path)
{
	std::ifstream input(path.c_str(), std::ios::binary);
	if (!input)
	{
		std::ostringstream oss;
		oss << "Failed to open '" << path << "': " << std::strerror(errno);
		throw std::runtime_error(oss.str());
	}

	input.seekg(0, std::ios::end);
	const std::streamoff fileSize = input.tellg();
	if (fileSize < 0)
	{
		throw std::runtime_error("Failed to determine file size.");
	}
	input.seekg(0, std::ios::beg);

	std::vector<unsigned char> data(static_cast<size_t>(fileSize));
	if (!data.empty())
	{
		input.read(reinterpret_cast<char*>(&data[0]), fileSize);
		if (!input)
		{
			throw std::runtime_error("Failed to read the complete file.");
		}
	}

	return data;
}

std::string vec3ToString(const float* value)
{
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(3)
		<< "(" << value[0] << ", " << value[1] << ", " << value[2] << ")";
	return oss.str();
}

LoadedNavMesh loadWrappedNavMesh(const std::string& path)
{
	const std::vector<unsigned char> fileData = readFile(path);
	if (fileData.size() < sizeof(NavMeshSetHeader))
	{
		throw std::runtime_error("File is smaller than NavMeshSetHeader.");
	}

	size_t offset = 0;
	const NavMeshSetHeader fileHeader =
		readStruct<NavMeshSetHeader>(&fileData[0], fileData.size(), offset, "NavMeshSetHeader");
	offset += sizeof(NavMeshSetHeader);

	if (fileHeader.version != 1)
	{
		throw std::runtime_error("Unsupported NavMeshSetHeader version.");
	}

	LoadedNavMesh loaded;
	loaded.navMesh = dtAllocNavMesh();
	if (!loaded.navMesh)
	{
		throw std::runtime_error("dtAllocNavMesh failed.");
	}

	const dtStatus initStatus = loaded.navMesh->init(&fileHeader.params);
	if (dtStatusFailed(initStatus))
	{
		throw std::runtime_error("dtNavMesh::init(params) failed.");
	}

	for (int i = 0; i < fileHeader.tileCount; ++i)
	{
		const NavMeshTileHeader tileHeader =
			readStruct<NavMeshTileHeader>(&fileData[0], fileData.size(), offset, "NavMeshTileHeader");
		offset += sizeof(NavMeshTileHeader);

		if (!tileHeader.tileRef || tileHeader.dataSize <= 0)
		{
			throw std::runtime_error("Invalid tile header encountered.");
		}
		if (offset + static_cast<size_t>(tileHeader.dataSize) > fileData.size())
		{
			throw std::runtime_error("Tile data overruns the file.");
		}

		unsigned char* tileData =
			static_cast<unsigned char*>(dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM));
		if (!tileData)
		{
			throw std::runtime_error("Failed to allocate tile data.");
		}

		std::memcpy(tileData, &fileData[offset], tileHeader.dataSize);
		offset += static_cast<size_t>(tileHeader.dataSize);

		dtStatus status = loaded.navMesh->addTile(
			tileData, tileHeader.dataSize, DT_TILE_FREE_DATA, tileHeader.tileRef, 0);
		if (dtStatusFailed(status))
		{
			dtFree(tileData);
			throw std::runtime_error("dtNavMesh::addTile failed.");
		}
	}

	loaded.navQuery = dtAllocNavMeshQuery();
	if (!loaded.navQuery)
	{
		throw std::runtime_error("dtAllocNavMeshQuery failed.");
	}

	const dtStatus queryStatus = loaded.navQuery->init(loaded.navMesh, 2048);
	if (dtStatusFailed(queryStatus))
	{
		throw std::runtime_error("dtNavMeshQuery::init failed.");
	}

	return loaded;
}

std::vector<PolyPoint> collectWalkablePolyCenters(const dtNavMesh& navMesh)
{
	std::vector<PolyPoint> points;

	for (int i = 0; i < navMesh.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navMesh.getTile(i);
		if (!tile || !tile->header)
		{
			continue;
		}

		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
			{
				continue;
			}

			PolyPoint point;
			point.ref = navMesh.getPolyRefBase(tile) | static_cast<dtPolyRef>(p);
			for (unsigned int v = 0; v < poly.vertCount; ++v)
			{
				const float* vert = &tile->verts[poly.verts[v] * 3];
				point.pos[0] += vert[0];
				point.pos[1] += vert[1];
				point.pos[2] += vert[2];
			}
			const float inv = 1.0f / static_cast<float>(poly.vertCount);
			point.pos[0] *= inv;
			point.pos[1] *= inv;
			point.pos[2] *= inv;
			points.push_back(point);
		}
	}

	return points;
}

void printUsage(const char* argv0)
{
	std::cout
		<< "Usage: " << argv0 << " <file.navmesh>\n"
		<< "Loads the KBEngine .navmesh wrapper and runs a small runtime query demo.\n";
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		printUsage(argv[0]);
		return argc == 1 ? 0 : 1;
	}

	try
	{
		const std::string path = argv[1];
		LoadedNavMesh loaded = loadWrappedNavMesh(path);
		std::vector<PolyPoint> points = collectWalkablePolyCenters(*loaded.navMesh);
		if (points.size() < 2)
		{
			throw std::runtime_error("Need at least two walkable polygons for demo.");
		}

		const PolyPoint& start = points.front();
		const PolyPoint& end = points.back();

		dtQueryFilter filter;
		filter.setIncludeFlags(0xffff);
		filter.setExcludeFlags(0);

		const float extents[3] = {2.f, 4.f, 2.f};

		dtPolyRef nearestStartRef = 0;
		dtPolyRef nearestEndRef = 0;
		float nearestStart[3];
		float nearestEnd[3];
		loaded.navQuery->findNearestPoly(start.pos, extents, &filter, &nearestStartRef, nearestStart);
		loaded.navQuery->findNearestPoly(end.pos, extents, &filter, &nearestEndRef, nearestEnd);

		dtPolyRef corridor[256];
		int corridorCount = 0;
		loaded.navQuery->findPath(
			nearestStartRef, nearestEndRef, nearestStart, nearestEnd, &filter,
			corridor, &corridorCount, 256);

		float straight[256 * 3];
		unsigned char straightFlags[256];
		dtPolyRef straightRefs[256];
		int straightCount = 0;
		loaded.navQuery->findStraightPath(
			nearestStart, nearestEnd, corridor, corridorCount,
			straight, straightFlags, straightRefs, &straightCount, 256);

		float desiredMove[3] =
		{
			nearestStart[0] + (nearestEnd[0] - nearestStart[0]) * 0.25f,
			nearestStart[1] + (nearestEnd[1] - nearestStart[1]) * 0.25f,
			nearestStart[2] + (nearestEnd[2] - nearestStart[2]) * 0.25f
		};

		float movedPos[3];
		dtPolyRef visited[16];
		int visitedCount = 0;
		loaded.navQuery->moveAlongSurface(
			nearestStartRef, nearestStart, desiredMove, &filter, movedPos, visited, &visitedCount, 16);

		float t = 0.f;
		float hitNormal[3] = {0.f, 0.f, 0.f};
		dtPolyRef rayPolys[256];
		int rayPolyCount = 0;
		loaded.navQuery->raycast(
			nearestStartRef, nearestStart, nearestEnd, &filter, &t, hitNormal, rayPolys, &rayPolyCount, 256);

		float wallDistance = 0.f;
		float wallHitPos[3] = {0.f, 0.f, 0.f};
		float wallHitNormal[3] = {0.f, 0.f, 0.f};
		loaded.navQuery->findDistanceToWall(
			nearestStartRef, nearestStart, 100.0f, &filter, &wallDistance, wallHitPos, wallHitNormal);

		float height = 0.f;
		loaded.navQuery->getPolyHeight(nearestStartRef, nearestStart, &height);

		std::cout << "File: " << path << "\n";
		std::cout << "Walkable polygon centers: " << points.size() << "\n";
		std::cout << "Start ref: " << nearestStartRef << ", pos=" << vec3ToString(nearestStart) << "\n";
		std::cout << "End ref: " << nearestEndRef << ", pos=" << vec3ToString(nearestEnd) << "\n";
		std::cout << "Path corridor polys: " << corridorCount << "\n";
		std::cout << "Straight path points: " << straightCount << "\n";
		for (int i = 0; i < straightCount; ++i)
		{
			std::cout << "  p" << i << " = "
				<< vec3ToString(&straight[i * 3])
				<< ", flag=" << static_cast<int>(straightFlags[i])
				<< ", ref=" << straightRefs[i] << "\n";
		}
		std::cout << "MoveAlongSurface result: " << vec3ToString(movedPos)
			<< ", visitedPolys=" << visitedCount << "\n";
		std::cout << "Raycast t=" << t
			<< ", normal=" << vec3ToString(hitNormal)
			<< ", visitedPolys=" << rayPolyCount << "\n";
		std::cout << "DistanceToWall distance=" << wallDistance
			<< ", hitPos=" << vec3ToString(wallHitPos)
			<< ", hitNormal=" << vec3ToString(wallHitNormal) << "\n";
		std::cout << "Start poly height: " << std::fixed << std::setprecision(3) << height << "\n";
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "navmesh_query_demo: " << ex.what() << "\n";
		return 1;
	}
}
