#include "navmesh_baker.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"

namespace KBEngine
{
namespace
{

struct ObjMesh
{
	std::vector<float> verts;
	std::vector<int> tris;
};

struct MeshBundle
{
	dtNavMesh* navMesh = 0;

	~MeshBundle()
	{
		if (navMesh)
		{
			dtFreeNavMesh(navMesh);
			navMesh = 0;
		}
	}
};

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

std::string trim(const std::string& s)
{
	std::size_t start = 0;
	while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
	{
		++start;
	}

	std::size_t end = s.size();
	while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
	{
		--end;
	}

	return s.substr(start, end - start);
}

int parseFaceIndex(const std::string& token, int vertCount)
{
	const std::size_t slash = token.find('/');
	const std::string indexToken = slash == std::string::npos ? token : token.substr(0, slash);
	if (indexToken.empty())
	{
		throw std::runtime_error("OBJ face token missing vertex index.");
	}

	const int index = std::atoi(indexToken.c_str());
	if (index == 0)
	{
		throw std::runtime_error("OBJ uses unsupported zero vertex index.");
	}

	if (index > 0)
	{
		return index - 1;
	}

	return vertCount + index;
}

ObjMesh loadObj(const std::string& path)
{
	std::ifstream input(path.c_str());
	if (!input)
	{
		std::ostringstream oss;
		oss << "Failed to open OBJ '" << path << "': " << std::strerror(errno);
		throw std::runtime_error(oss.str());
	}

	ObjMesh mesh;
	std::string line;
	while (std::getline(input, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == '#')
		{
			continue;
		}

		std::istringstream iss(line);
		std::string type;
		iss >> type;
		if (type == "v")
		{
			float x = 0.f;
			float y = 0.f;
			float z = 0.f;
			iss >> x >> y >> z;
			mesh.verts.push_back(x);
			mesh.verts.push_back(y);
			mesh.verts.push_back(z);
		}
		else if (type == "f")
		{
			std::vector<int> face;
			std::string token;
			const int vertCount = static_cast<int>(mesh.verts.size() / 3);
			while (iss >> token)
			{
				const int index = parseFaceIndex(token, vertCount);
				if (index < 0 || index >= vertCount)
				{
					throw std::runtime_error("OBJ face index out of range.");
				}
				face.push_back(index);
			}

			if (face.size() < 3)
			{
				continue;
			}

			for (std::size_t i = 1; i + 1 < face.size(); ++i)
			{
				mesh.tris.push_back(face[0]);
				mesh.tris.push_back(face[i]);
				mesh.tris.push_back(face[i + 1]);
			}
		}
	}

	if (mesh.verts.empty() || mesh.tris.empty())
	{
		throw std::runtime_error("OBJ did not contain usable triangles.");
	}

	return mesh;
}

bool buildSoloNavMesh(const ObjMesh& mesh, const NavmeshBakeSettings& settings, rcContext& ctx,
	MeshBundle& outBundle, NavmeshBakeResult* outResult)
{
	float bmin[3];
	float bmax[3];
	rcCalcBounds(&mesh.verts[0], static_cast<int>(mesh.verts.size() / 3), bmin, bmax);

	rcConfig cfg;
	std::memset(&cfg, 0, sizeof(cfg));
	cfg.cs = settings.cellSize;
	cfg.ch = settings.cellHeight;
	cfg.walkableSlopeAngle = settings.agentMaxSlope;
	cfg.walkableHeight = static_cast<int>(std::ceil(settings.agentHeight / cfg.ch));
	cfg.walkableClimb = static_cast<int>(std::floor(settings.agentMaxClimb / cfg.ch));
	cfg.walkableRadius = static_cast<int>(std::ceil(settings.agentRadius / cfg.cs));
	cfg.maxEdgeLen = static_cast<int>(settings.edgeMaxLen / settings.cellSize);
	cfg.maxSimplificationError = settings.edgeMaxError;
	cfg.minRegionArea = static_cast<int>(rcSqr(settings.regionMinSize));
	cfg.mergeRegionArea = static_cast<int>(rcSqr(settings.regionMergeSize));
	cfg.maxVertsPerPoly = settings.vertsPerPoly;
	cfg.detailSampleDist = settings.detailSampleDist < 0.9f ? 0.f : settings.cellSize * settings.detailSampleDist;
	cfg.detailSampleMaxError = settings.cellHeight * settings.detailSampleMaxError;
	rcVcopy(cfg.bmin, bmin);
	rcVcopy(cfg.bmax, bmax);
	rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

	if (outResult)
	{
		outResult->walkableHeightVoxels = cfg.walkableHeight;
		outResult->walkableClimbVoxels = cfg.walkableClimb;
		outResult->walkableRadiusVoxels = cfg.walkableRadius;
	}

	std::vector<unsigned char> triAreas(mesh.tris.size() / 3, 0);
	rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, &mesh.verts[0], static_cast<int>(mesh.verts.size() / 3),
		&mesh.tris[0], static_cast<int>(mesh.tris.size() / 3), &triAreas[0]);

	rcHeightfield* solid = rcAllocHeightfield();
	rcCompactHeightfield* chf = rcAllocCompactHeightfield();
	rcContourSet* cset = rcAllocContourSet();
	rcPolyMesh* pmesh = rcAllocPolyMesh();
	rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();

	if (!solid || !chf || !cset || !pmesh || !dmesh)
	{
		rcFreeHeightField(solid);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
		return false;
	}

	bool ok = true;
	ok = ok && rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch);
	if (ok)
	{
		rcRasterizeTriangles(&ctx, &mesh.verts[0], static_cast<int>(mesh.verts.size() / 3),
			&mesh.tris[0], &triAreas[0], static_cast<int>(mesh.tris.size() / 3), *solid, cfg.walkableClimb);
	}
	if (ok)
	{
		rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
		rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
		rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);
	}
	ok = ok && rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf);
	ok = ok && rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf);
	ok = ok && rcBuildDistanceField(&ctx, *chf);
	ok = ok && rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea);
	ok = ok && rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset);
	ok = ok && rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh);
	ok = ok && rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh);

	if (!ok)
	{
		rcFreeHeightField(solid);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
		return false;
	}

	for (int i = 0; i < pmesh->npolys; ++i)
	{
		if (pmesh->areas[i] == RC_WALKABLE_AREA)
		{
			pmesh->areas[i] = 0;
		}
		if (pmesh->areas[i] == 0)
		{
			pmesh->flags[i] = 1;
		}
	}

	dtNavMeshCreateParams params;
	std::memset(&params, 0, sizeof(params));
	params.verts = pmesh->verts;
	params.vertCount = pmesh->nverts;
	params.polys = pmesh->polys;
	params.polyAreas = pmesh->areas;
	params.polyFlags = pmesh->flags;
	params.polyCount = pmesh->npolys;
	params.nvp = pmesh->nvp;
	params.detailMeshes = dmesh->meshes;
	params.detailVerts = dmesh->verts;
	params.detailVertsCount = dmesh->nverts;
	params.detailTris = dmesh->tris;
	params.detailTriCount = dmesh->ntris;
	params.walkableHeight = settings.agentHeight;
	params.walkableRadius = settings.agentRadius;
	params.walkableClimb = settings.agentMaxClimb;
	rcVcopy(params.bmin, pmesh->bmin);
	rcVcopy(params.bmax, pmesh->bmax);
	params.cs = cfg.cs;
	params.ch = cfg.ch;
	params.buildBvTree = true;

	unsigned char* navData = 0;
	int navDataSize = 0;
	if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
	{
		rcFreeHeightField(solid);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
		return false;
	}

	dtNavMesh* navMesh = dtAllocNavMesh();
	if (!navMesh)
	{
		dtFree(navData);
		rcFreeHeightField(solid);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
		return false;
	}

	if (dtStatusFailed(navMesh->init(navData, navDataSize, DT_TILE_FREE_DATA)))
	{
		dtFreeNavMesh(navMesh);
		rcFreeHeightField(solid);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
		return false;
	}

	outBundle.navMesh = navMesh;

	rcFreeHeightField(solid);
	rcFreeCompactHeightfield(chf);
	rcFreeContourSet(cset);
	rcFreePolyMesh(pmesh);
	rcFreePolyMeshDetail(dmesh);
	return true;
}

struct SvgBounds
{
	float minX;
	float minZ;
	float maxX;
	float maxZ;
};

bool computeSvgBounds(const dtNavMesh& navMesh, SvgBounds& out)
{
	bool hasAny = false;
	out.minX = out.minZ = std::numeric_limits<float>::max();
	out.maxX = out.maxZ = -std::numeric_limits<float>::max();

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

			for (unsigned int v = 0; v < poly.vertCount; ++v)
			{
				const float* vert = &tile->verts[poly.verts[v] * 3];
				out.minX = std::min(out.minX, vert[0]);
				out.minZ = std::min(out.minZ, vert[2]);
				out.maxX = std::max(out.maxX, vert[0]);
				out.maxZ = std::max(out.maxZ, vert[2]);
				hasAny = true;
			}
		}
	}

	return hasAny;
}

bool exportTopDownSvg(const dtNavMesh& navMesh, const std::string& outPath)
{
	if (outPath.empty())
	{
		return true;
	}

	SvgBounds bounds;
	if (!computeSvgBounds(navMesh, bounds))
	{
		return false;
	}

	const float padding = 10.f;
	const float width = std::max(1.f, bounds.maxX - bounds.minX);
	const float height = std::max(1.f, bounds.maxZ - bounds.minZ);
	const float scale = 1600.f / std::max(width, height);
	const float canvasW = width * scale + padding * 2.f;
	const float canvasH = height * scale + padding * 2.f;

	std::ofstream out(outPath.c_str());
	if (!out)
	{
		return false;
	}

	out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << canvasW
	    << "\" height=\"" << canvasH << "\" viewBox=\"0 0 " << canvasW << " " << canvasH << "\">\n";
	out << "  <rect width=\"100%\" height=\"100%\" fill=\"#f4f4f4\"/>\n";
	out << "  <g fill=\"#bfc6cd\" stroke=\"#41464d\" stroke-width=\"0.8\">\n";

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

			out << "    <polygon points=\"";
			for (unsigned int v = 0; v < poly.vertCount; ++v)
			{
				const float* vert = &tile->verts[poly.verts[v] * 3];
				const float x = padding + (vert[0] - bounds.minX) * scale;
				const float y = padding + (bounds.maxZ - vert[2]) * scale;
				if (v)
				{
					out << ' ';
				}
				out << x << ',' << y;
			}
			out << "\"/>\n";
		}
	}

	out << "  </g>\n";
	out << "</svg>\n";
	return true;
}

bool saveWrappedNavMesh(const dtNavMesh& navMesh, const std::string& outPath)
{
	const dtNavMeshParams* params = navMesh.getParams();
	if (!params)
	{
		return false;
	}

	int tileCount = 0;
	for (int i = 0; i < navMesh.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navMesh.getTile(i);
		if (tile && tile->header && tile->data && tile->dataSize > 0)
		{
			++tileCount;
		}
	}

	std::ofstream out(outPath.c_str(), std::ios::binary);
	if (!out)
	{
		return false;
	}

	NavMeshSetHeader header;
	std::memset(&header, 0, sizeof(header));
	header.version = 1;
	header.tileCount = tileCount;
	header.params = *params;
	out.write(reinterpret_cast<const char*>(&header), sizeof(header));

	for (int i = 0; i < navMesh.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navMesh.getTile(i);
		if (!tile || !tile->header || !tile->data || tile->dataSize <= 0)
		{
			continue;
		}

		NavMeshTileHeader tileHeader;
		tileHeader.tileRef = navMesh.getTileRef(tile);
		tileHeader.dataSize = tile->dataSize;
		out.write(reinterpret_cast<const char*>(&tileHeader), sizeof(tileHeader));
		out.write(reinterpret_cast<const char*>(tile->data), tile->dataSize);
	}

	return static_cast<bool>(out);
}

int countWalkablePolys(const dtNavMesh& navMesh)
{
	int total = 0;
	for (int i = 0; i < navMesh.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navMesh.getTile(i);
		if (!tile || !tile->header)
		{
			continue;
		}

		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			if (tile->polys[p].getType() != DT_POLYTYPE_OFFMESH_CONNECTION)
			{
				++total;
			}
		}
	}
	return total;
}

int countTiles(const dtNavMesh& navMesh)
{
	int total = 0;
	for (int i = 0; i < navMesh.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navMesh.getTile(i);
		if (tile && tile->header)
		{
			++total;
		}
	}
	return total;
}

void resetResult(NavmeshBakeResult* outResult)
{
	if (!outResult)
	{
		return;
	}

	*outResult = NavmeshBakeResult();
}

}

bool bakeNavmeshFromObj(const NavmeshBakeRequest& request, NavmeshBakeResult* outResult)
{
	resetResult(outResult);

	try
	{
		if (request.inputObjPath.empty())
		{
			throw std::runtime_error("inputObjPath is empty.");
		}
		if (request.outputNavmeshPath.empty())
		{
			throw std::runtime_error("outputNavmeshPath is empty.");
		}

		const ObjMesh mesh = loadObj(request.inputObjPath);
		rcContext ctx(true);
		MeshBundle bundle;

		if (!buildSoloNavMesh(mesh, request.settings, ctx, bundle, outResult))
		{
			throw std::runtime_error("Failed to build navmesh.");
		}

		if (!saveWrappedNavMesh(*bundle.navMesh, request.outputNavmeshPath))
		{
			throw std::runtime_error("Failed to write wrapped .navmesh.");
		}

		if (!exportTopDownSvg(*bundle.navMesh, request.outputSvgPath))
		{
			throw std::runtime_error("Failed to export SVG.");
		}

		if (outResult)
		{
			outResult->ok = true;
			outResult->vertexCount = mesh.verts.size() / 3;
			outResult->triangleCount = mesh.tris.size() / 3;
			outResult->tileCount = countTiles(*bundle.navMesh);
			outResult->walkablePolyCount = countWalkablePolys(*bundle.navMesh);
		}

		return true;
	}
	catch (const std::exception& ex)
	{
		if (outResult)
		{
			outResult->error = ex.what();
		}
		return false;
	}
}

}
