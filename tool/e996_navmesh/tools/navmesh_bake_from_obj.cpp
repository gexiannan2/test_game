#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "navmesh_baker.h"

namespace
{

bool parseFloatValue(const char* option, const char* value, float& out)
{
	char* end = 0;
	const float parsed = std::strtof(value, &end);
	if (!end || *end != '\0')
	{
		std::ostringstream oss;
		oss << "Invalid value for " << option << ": '" << value << "'";
		throw std::runtime_error(oss.str());
	}

	out = parsed;
	return true;
}

bool parseIntValue(const char* option, const char* value, int& out)
{
	char* end = 0;
	const long parsed = std::strtol(value, &end, 10);
	if (!end || *end != '\0')
	{
		std::ostringstream oss;
		oss << "Invalid value for " << option << ": '" << value << "'";
		throw std::runtime_error(oss.str());
	}

	out = static_cast<int>(parsed);
	return true;
}

void applyOption(KBEngine::NavmeshBakeSettings& settings, const std::string& option, const char* value)
{
	if (option == "--cell-size")
	{
		parseFloatValue(option.c_str(), value, settings.cellSize);
	}
	else if (option == "--cell-height")
	{
		parseFloatValue(option.c_str(), value, settings.cellHeight);
	}
	else if (option == "--agent-height")
	{
		parseFloatValue(option.c_str(), value, settings.agentHeight);
	}
	else if (option == "--agent-radius")
	{
		parseFloatValue(option.c_str(), value, settings.agentRadius);
	}
	else if (option == "--agent-max-climb")
	{
		parseFloatValue(option.c_str(), value, settings.agentMaxClimb);
	}
	else if (option == "--agent-max-slope")
	{
		parseFloatValue(option.c_str(), value, settings.agentMaxSlope);
	}
	else if (option == "--region-min-size")
	{
		parseFloatValue(option.c_str(), value, settings.regionMinSize);
	}
	else if (option == "--region-merge-size")
	{
		parseFloatValue(option.c_str(), value, settings.regionMergeSize);
	}
	else if (option == "--edge-max-len")
	{
		parseFloatValue(option.c_str(), value, settings.edgeMaxLen);
	}
	else if (option == "--edge-max-error")
	{
		parseFloatValue(option.c_str(), value, settings.edgeMaxError);
	}
	else if (option == "--verts-per-poly")
	{
		parseIntValue(option.c_str(), value, settings.vertsPerPoly);
	}
	else if (option == "--detail-sample-dist")
	{
		parseFloatValue(option.c_str(), value, settings.detailSampleDist);
	}
	else if (option == "--detail-sample-max-error")
	{
		parseFloatValue(option.c_str(), value, settings.detailSampleMaxError);
	}
	else
	{
		std::ostringstream oss;
		oss << "Unknown option: " << option;
		throw std::runtime_error(oss.str());
	}
}

void parseOptions(int argc, char** argv, int optionStart, KBEngine::NavmeshBakeSettings& settings)
{
	for (int i = optionStart; i < argc; ++i)
	{
		const std::string option = argv[i];
		if (option == "--help")
		{
			continue;
		}

		if (i + 1 >= argc)
		{
			std::ostringstream oss;
			oss << "Missing value for option: " << option;
			throw std::runtime_error(oss.str());
		}

		applyOption(settings, option, argv[++i]);
	}
}
void printSettings(const KBEngine::NavmeshBakeSettings& settings)
{
	std::cout << "Bake settings\n";
	std::cout << "  cellSize=" << settings.cellSize << "\n";
	std::cout << "  cellHeight=" << settings.cellHeight << "\n";
	std::cout << "  agentHeight=" << settings.agentHeight << "\n";
	std::cout << "  agentRadius=" << settings.agentRadius << "\n";
	std::cout << "  agentMaxClimb=" << settings.agentMaxClimb << "\n";
	std::cout << "  agentMaxSlope=" << settings.agentMaxSlope << "\n";
	std::cout << "  regionMinSize=" << settings.regionMinSize << "\n";
	std::cout << "  regionMergeSize=" << settings.regionMergeSize << "\n";
	std::cout << "  edgeMaxLen=" << settings.edgeMaxLen << "\n";
	std::cout << "  edgeMaxError=" << settings.edgeMaxError << "\n";
	std::cout << "  vertsPerPoly=" << settings.vertsPerPoly << "\n";
	std::cout << "  detailSampleDist=" << settings.detailSampleDist << "\n";
	std::cout << "  detailSampleMaxError=" << settings.detailSampleMaxError << "\n";
}

void printUsage(const char* argv0)
{
	std::cout
		<< "Usage: " << argv0 << " <input.obj> <output.navmesh> [output.svg] [options]\n"
		<< "Builds navmesh with navigation Recast/Detour sources, writes wrapped .navmesh,\n"
		<< "and optionally exports a top-down SVG.\n"
		<< "Options:\n"
		<< "  --cell-size <float>\n"
		<< "  --cell-height <float>\n"
		<< "  --agent-height <float>\n"
		<< "  --agent-radius <float>\n"
		<< "  --agent-max-climb <float>\n"
		<< "  --agent-max-slope <float>\n"
		<< "  --region-min-size <float>\n"
		<< "  --region-merge-size <float>\n"
		<< "  --edge-max-len <float>\n"
		<< "  --edge-max-error <float>\n"
		<< "  --verts-per-poly <int>\n"
		<< "  --detail-sample-dist <float>\n"
		<< "  --detail-sample-max-error <float>\n";
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::string(argv[1]) == "--help")
	{
		printUsage(argv[0]);
		return 0;
	}

	if (argc < 3)
	{
		printUsage(argv[0]);
		return argc == 1 ? 0 : 1;
	}

	try
	{
		KBEngine::NavmeshBakeRequest request;
		request.inputObjPath = argv[1];
		request.outputNavmeshPath = argv[2];

		int optionStart = 3;
		if (argc >= 4)
		{
			const std::string maybeSvg = argv[3];
			if (maybeSvg.rfind("--", 0) != 0)
			{
				request.outputSvgPath = maybeSvg;
				optionStart = 4;
			}
		}

		parseOptions(argc, argv, optionStart, request.settings);
		printSettings(request.settings);

		KBEngine::NavmeshBakeResult result;
		if (!KBEngine::bakeNavmeshFromObj(request, &result))
		{
			throw std::runtime_error(result.error.empty() ? "Failed to build navmesh." : result.error);
		}

		std::cout << "Loaded OBJ: " << request.inputObjPath << "\n";
		std::cout << "  vertices=" << result.vertexCount << "\n";
		std::cout << "  triangles=" << result.triangleCount << "\n";
		std::cout << "Built navmesh\n";
		std::cout << "  tiles=" << result.tileCount << "\n";
		std::cout << "  walkablePolys=" << result.walkablePolyCount << "\n";
		std::cout << "  walkableHeightVoxels=" << result.walkableHeightVoxels << "\n";
		std::cout << "  walkableClimbVoxels=" << result.walkableClimbVoxels << "\n";
		std::cout << "  walkableRadiusVoxels=" << result.walkableRadiusVoxels << "\n";
		std::cout << "Navmesh: " << request.outputNavmeshPath << "\n";
		if (!request.outputSvgPath.empty())
		{
			std::cout << "SVG: " << request.outputSvgPath << "\n";
		}
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "u3d_navmesh_bake_verify: " << ex.what() << "\n";
		return 1;
	}
}
