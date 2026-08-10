#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DetourCommon.h"
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

struct Point
{
	float x;
	float y;
	float z;
};

struct LoadedNavMesh
{
	dtNavMesh* navMesh;
	dtNavMeshQuery* navQuery;

	LoadedNavMesh() : navMesh(NULL), navQuery(NULL) {}

	~LoadedNavMesh()
	{
		if (navQuery)
		{
			dtFreeNavMeshQuery(navQuery);
		}
		if (navMesh)
		{
			dtFreeNavMesh(navMesh);
		}
	}
};

template <typename T>
T readStruct(const unsigned char* data, size_t size, size_t offset, const char* what)
{
	if (offset + sizeof(T) > size)
	{
		std::ostringstream oss;
		oss << "unexpected EOF while reading " << what;
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
		oss << "failed to open '" << path << "': " << std::strerror(errno);
		throw std::runtime_error(oss.str());
	}

	input.seekg(0, std::ios::end);
	const std::streamoff fileSize = input.tellg();
	input.seekg(0, std::ios::beg);

	std::vector<unsigned char> data(static_cast<size_t>(fileSize));
	if (!data.empty())
	{
		input.read(reinterpret_cast<char*>(&data[0]), fileSize);
	}
	return data;
}

LoadedNavMesh loadWrappedNavMesh(const std::string& path)
{
	const std::vector<unsigned char> fileData = readFile(path);
	if (fileData.size() < sizeof(NavMeshSetHeader))
	{
		throw std::runtime_error("file is smaller than NavMeshSetHeader");
	}

	size_t offset = 0;
	const NavMeshSetHeader header =
		readStruct<NavMeshSetHeader>(&fileData[0], fileData.size(), offset, "NavMeshSetHeader");
	offset += sizeof(NavMeshSetHeader);

	if (header.version != 1)
	{
		throw std::runtime_error("unsupported navmesh wrapper version");
	}

	LoadedNavMesh loaded;
	loaded.navMesh = dtAllocNavMesh();
	if (!loaded.navMesh)
	{
		throw std::runtime_error("dtAllocNavMesh failed");
	}
	if (dtStatusFailed(loaded.navMesh->init(&header.params)))
	{
		throw std::runtime_error("dtNavMesh::init(params) failed");
	}

	for (int i = 0; i < header.tileCount; ++i)
	{
		const NavMeshTileHeader tileHeader =
			readStruct<NavMeshTileHeader>(&fileData[0], fileData.size(), offset, "NavMeshTileHeader");
		offset += sizeof(NavMeshTileHeader);

		if (!tileHeader.tileRef || tileHeader.dataSize <= 0 ||
			offset + static_cast<size_t>(tileHeader.dataSize) > fileData.size())
		{
			throw std::runtime_error("invalid tile header");
		}

		unsigned char* tileData =
			static_cast<unsigned char*>(dtAlloc(tileHeader.dataSize, DT_ALLOC_PERM));
		if (!tileData)
		{
			throw std::runtime_error("dtAlloc tile data failed");
		}
		std::memcpy(tileData, &fileData[offset], tileHeader.dataSize);
		offset += static_cast<size_t>(tileHeader.dataSize);

		if (dtStatusFailed(loaded.navMesh->addTile(
			tileData, tileHeader.dataSize, DT_TILE_FREE_DATA, tileHeader.tileRef, NULL)))
		{
			dtFree(tileData);
			throw std::runtime_error("dtNavMesh::addTile failed");
		}
	}

	loaded.navQuery = dtAllocNavMeshQuery();
	if (!loaded.navQuery)
	{
		throw std::runtime_error("dtAllocNavMeshQuery failed");
	}
	if (dtStatusFailed(loaded.navQuery->init(loaded.navMesh, 2048)))
	{
		throw std::runtime_error("dtNavMeshQuery::init failed");
	}
	return loaded;
}

std::vector<Point> loadPath(const std::string& path)
{
	std::ifstream input(path.c_str());
	if (!input)
	{
		std::ostringstream oss;
		oss << "failed to open path file '" << path << "': " << std::strerror(errno);
		throw std::runtime_error(oss.str());
	}

	std::vector<Point> points;
	std::string line;
	while (std::getline(input, line))
	{
		if (line.empty() || line.find('=') != std::string::npos)
		{
			continue;
		}

		for (size_t i = 0; i < line.size(); ++i)
		{
			if (line[i] == ',')
			{
				line[i] = ' ';
			}
		}

		std::istringstream iss(line);
		int index = 0;
		Point point;
		if (iss >> index >> point.x >> point.y >> point.z)
		{
			points.push_back(point);
		}
	}
	return points;
}

float dist2D(const float* a, const Point& b)
{
	const float dx = a[0] - b.x;
	const float dz = a[2] - b.z;
	return std::sqrt(dx * dx + dz * dz);
}

float dist3D(const float* a, const Point& b)
{
	const float dx = a[0] - b.x;
	const float dy = a[1] - b.y;
	const float dz = a[2] - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float distPointSegment2D(const Point& p, const float* a, const float* b)
{
	const float vx = b[0] - a[0];
	const float vz = b[2] - a[2];
	const float wx = p.x - a[0];
	const float wz = p.z - a[2];
	const float len2 = vx * vx + vz * vz;
	float t = len2 > 0.f ? (wx * vx + wz * vz) / len2 : 0.f;
	if (t < 0.f)
	{
		t = 0.f;
	}
	else if (t > 1.f)
	{
		t = 1.f;
	}
	const float cx = a[0] + vx * t;
	const float cz = a[2] + vz * t;
	const float dx = p.x - cx;
	const float dz = p.z - cz;
	return std::sqrt(dx * dx + dz * dz);
}

void getDetailTriVerts(const dtMeshTile* tile, const dtPoly* poly, int polyIndex, int triIndex, const float* out[3])
{
	const dtPolyDetail& detail = tile->detailMeshes[polyIndex];
	const unsigned char* tri = &tile->detailTris[(detail.triBase + triIndex) * 4];
	for (int i = 0; i < 3; ++i)
	{
		if (tri[i] < poly->vertCount)
		{
			out[i] = &tile->verts[poly->verts[tri[i]] * 3];
		}
		else
		{
			out[i] = &tile->detailVerts[(detail.vertBase + (tri[i] - poly->vertCount)) * 3];
		}
	}
}

void printUsage(const char* argv0)
{
	std::cout << "Usage: " << argv0 << " <input.navmesh> <path.txt>\n";
}

bool isAdjacent(const dtNavMesh& navMesh, dtPolyRef from, dtPolyRef to)
{
	if (!from || !to)
	{
		return false;
	}
	if (from == to)
	{
		return true;
	}

	const dtMeshTile* tile = NULL;
	const dtPoly* poly = NULL;
	if (dtStatusFailed(navMesh.getTileAndPolyByRef(from, &tile, &poly)))
	{
		return false;
	}

	for (unsigned int linkIndex = poly->firstLink; linkIndex != DT_NULL_LINK; linkIndex = tile->links[linkIndex].next)
	{
		if (tile->links[linkIndex].ref == to)
		{
			return true;
		}
	}
	return false;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		printUsage(argv[0]);
		return argc == 1 ? 0 : 1;
	}

	try
	{
		LoadedNavMesh loaded = loadWrappedNavMesh(argv[1]);
		const std::vector<Point> points = loadPath(argv[2]);

		dtQueryFilter filter;
		filter.setIncludeFlags(0xffff);
		filter.setExcludeFlags(0);
		const float extents[3] = {2.f, 4.f, 2.f};

		std::cout << std::fixed << std::setprecision(6);
		std::cout << "points=" << points.size() << "\n";
		std::cout << "index,polyRef,connectedToPrev,nearestPolyVert2D,nearestDetailVert2D,nearestDetailEdge2D,detailHeightDelta,segmentRaycastT\n";

		dtPolyRef prevRefForAdjacency = 0;

		for (size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex)
		{
			const Point& point = points[pointIndex];
			const float pos[3] = {point.x, point.y, point.z};
			dtPolyRef ref = 0;
			float nearest[3] = {0.f, 0.f, 0.f};
			loaded.navQuery->findNearestPoly(pos, extents, &filter, &ref, nearest);

			float minPolyVert2D = std::numeric_limits<float>::max();
			float minDetailVert2D = std::numeric_limits<float>::max();
			float minDetailEdge2D = std::numeric_limits<float>::max();
			float detailHeight = std::numeric_limits<float>::quiet_NaN();

			if (ref)
			{
				const dtMeshTile* tile = NULL;
				const dtPoly* poly = NULL;
				if (dtStatusSucceed(loaded.navMesh->getTileAndPolyByRef(ref, &tile, &poly)))
				{
					const int polyIndex = static_cast<int>(poly - tile->polys);
					for (unsigned int v = 0; v < poly->vertCount; ++v)
					{
						const float* vert = &tile->verts[poly->verts[v] * 3];
						minPolyVert2D = std::min(minPolyVert2D, dist2D(vert, point));
						minDetailVert2D = std::min(minDetailVert2D, dist2D(vert, point));
					}

					const dtPolyDetail& detail = tile->detailMeshes[polyIndex];
					for (unsigned int v = 0; v < detail.vertCount; ++v)
					{
						const float* vert = &tile->detailVerts[(detail.vertBase + v) * 3];
						minDetailVert2D = std::min(minDetailVert2D, dist2D(vert, point));
					}

					for (unsigned int t = 0; t < detail.triCount; ++t)
					{
						const float* verts[3];
						getDetailTriVerts(tile, poly, polyIndex, static_cast<int>(t), verts);
						for (int edge = 0; edge < 3; ++edge)
						{
							minDetailEdge2D = std::min(minDetailEdge2D,
								distPointSegment2D(point, verts[edge], verts[(edge + 1) % 3]));
						}
					}

					float h = 0.f;
					if (dtStatusSucceed(loaded.navQuery->getPolyHeight(ref, pos, &h)))
					{
						detailHeight = h;
					}
				}
			}

			float raycastT = -1.f;
			if (pointIndex > 0)
			{
				const Point& prevPoint = points[pointIndex - 1];
				const float prev[3] = {prevPoint.x, prevPoint.y, prevPoint.z};
				dtPolyRef prevRef = 0;
				float prevNearest[3] = {0.f, 0.f, 0.f};
				loaded.navQuery->findNearestPoly(prev, extents, &filter, &prevRef, prevNearest);
				if (prevRef)
				{
					float hitNormal[3] = {0.f, 0.f, 0.f};
					dtPolyRef rayPolys[256];
					int rayPolyCount = 0;
					raycastT = 0.f;
					loaded.navQuery->raycast(prevRef, prev, pos, &filter,
						&raycastT, hitNormal, rayPolys, &rayPolyCount, 256);
				}
			}

			std::cout << pointIndex << ','
				<< ref << ','
				<< (pointIndex == 0 ? 1 : (isAdjacent(*loaded.navMesh, prevRefForAdjacency, ref) ? 1 : 0)) << ','
				<< minPolyVert2D << ','
				<< minDetailVert2D << ','
				<< minDetailEdge2D << ','
				<< (std::isnan(detailHeight) ? 999999.f : point.y - detailHeight) << ','
				<< raycastT
				<< "\n";
			prevRefForAdjacency = ref;
		}
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "navmesh_path_compare: " << ex.what() << "\n";
		return 1;
	}
}
