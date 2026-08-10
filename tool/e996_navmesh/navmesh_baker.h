#ifndef KBE_NAVMESH_BAKER_H
#define KBE_NAVMESH_BAKER_H

#include <cstddef>
#include <string>

namespace KBEngine
{

struct NavmeshBakeSettings
{
	float cellSize = 0.3f;
	float cellHeight = 0.2f;
	float agentHeight = 2.0f;
	float agentRadius = 0.5f;
	float agentMaxClimb = 0.4f;
	float agentMaxSlope = 45.0f;
	float regionMinSize = 8.0f;
	float regionMergeSize = 20.0f;
	float edgeMaxLen = 12.0f;
	float edgeMaxError = 1.3f;
	int vertsPerPoly = 6;
	float detailSampleDist = 6.0f;
	float detailSampleMaxError = 1.0f;
};

struct NavmeshBakeRequest
{
	std::string inputObjPath;
	std::string outputNavmeshPath;
	std::string outputSvgPath;
	NavmeshBakeSettings settings;
};

struct NavmeshBakeResult
{
	bool ok = false;
	std::string error;
	std::size_t vertexCount = 0;
	std::size_t triangleCount = 0;
	int tileCount = 0;
	int walkablePolyCount = 0;
	int walkableHeightVoxels = 0;
	int walkableClimbVoxels = 0;
	int walkableRadiusVoxels = 0;
};

bool bakeNavmeshFromObj(const NavmeshBakeRequest& request, NavmeshBakeResult* outResult);

}

#endif
