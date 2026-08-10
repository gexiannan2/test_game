#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "depend/resmgr/resmgr.h"
#include "navigation.h"
#include "navigation_handle.h"

namespace
{

const char* navTypeToString(KBEngine::NavigationHandle::NAV_TYPE type)
{
	switch (type)
	{
	case KBEngine::NavigationHandle::NAV_MESH:
		return "NAV_MESH";
	default:
		return "NAV_UNKNOWN";
	}
}

float parseFloatValue(const char* label, const char* text)
{
	char* end = NULL;
	const float value = std::strtof(text, &end);
	if (end == NULL || *end != '\0')
	{
		std::ostringstream oss;
		oss << "invalid " << label << ": " << text;
		throw std::runtime_error(oss.str());
	}

	return value;
}

Position3D parsePoint(const char* xText, const char* yText, const char* zText, const char* prefix)
{
	Position3D point;
	std::string labelPrefix(prefix);
	point.x = parseFloatValue((labelPrefix + "X").c_str(), xText);
	point.y = parseFloatValue((labelPrefix + "Y").c_str(), yText);
	point.z = parseFloatValue((labelPrefix + "Z").c_str(), zText);
	return point;
}

void printPoint(const char* label, const Position3D& point)
{
	std::cout << label << ": "
		<< "(" << point.x << ", " << point.y << ", " << point.z << ")\n";
}

bool writePathFile(const std::string& path,
	const std::string& navName,
	const Position3D& start,
	const Position3D& end,
	const std::vector<Position3D>& points)
{
	std::ofstream out(path.c_str());
	if (!out)
	{
		return false;
	}

	out << std::setprecision(12);
	out << "navmesh=" << navName << "\n";
	out << "start=" << start.x << "," << start.y << "," << start.z << "\n";
	out << "end=" << end.x << "," << end.y << "," << end.z << "\n";
	out << "path_count=" << points.size() << "\n";
	for (size_t index = 0; index < points.size(); ++index)
	{
		out << index
			<< "," << points[index].x
			<< "," << points[index].y
			<< "," << points[index].z << "\n";
	}

	return static_cast<bool>(out);
}

void printUsage(const char* argv0)
{
	std::cout
		<< "Usage:\n"
		<< "  " << argv0 << " <startX> <startY> <startZ> <endX> <endY> <endZ> <output.txt>\n"
		<< "  " << argv0 << " <navName> <startX> <startY> <startZ> <endX> <endY> <endZ> <output.txt>\n"
		<< "\n"
		<< "If navName is omitted, 101_nav is used by default.\n";
}

} // namespace

int main(int argc, char** argv)
{
	std::cout << std::setprecision(12);

	if (argc != 8 && argc != 9)
	{
		printUsage(argv[0]);
		return argc == 1 ? 0 : 1;
	}

	try
	{
		std::string navName = "101_nav";
		int argOffset = 1;
		if (argc == 9)
		{
			navName = argv[1];
			argOffset = 2;
		}

		const Position3D start = parsePoint(
			argv[argOffset],
			argv[argOffset + 1],
			argv[argOffset + 2],
			"start");
		const Position3D end = parsePoint(
			argv[argOffset + 3],
			argv[argOffset + 4],
			argv[argOffset + 5],
			"end");
		const std::string outputPath = argv[argOffset + 6];

		KBEngine::Resmgr resmgr;
		KBEngine::Navigation navigation;

		KBEngine::NavigationHandlePtr nav =
			KBEngine::Navigation::getSingleton().loadNavigation(navName);

		if (!nav)
		{
			std::cerr << "loadNavigation failed: " << navName << "\n";
			std::cerr << "expected local test file: res/" << navName << ".navmesh\n";
			return 1;
		}

		std::cout << "loadNavigation ok: " << navName << "\n";
		std::cout << "handle type: " << navTypeToString(nav->type()) << "\n";
		std::cout << "local test path: res/" << navName << ".navmesh\n";

		if (nav->type() != KBEngine::NavigationHandle::NAV_MESH)
		{
			std::cerr << "current handle is not NAV_MESH, skip 3d path test\n";
			return 1;
		}

		std::vector<Position3D> paths;
		const int count = nav->findStraightPath(0, start, end, paths);

		printPoint("start", start);
		printPoint("end", end);
		std::cout << "findStraightPath count: " << count << "\n";

		if (count < 0)
		{
			std::cerr << "findStraightPath failed with code: " << count << "\n";
			return 1;
		}

		for (size_t index = 0; index < paths.size(); ++index)
		{
			std::cout << "path[" << index << "]: "
				<< "(" << paths[index].x << ", " << paths[index].y << ", " << paths[index].z << ")\n";
		}

		if (!writePathFile(outputPath, navName, start, end, paths))
		{
			std::cerr << "failed to write path file '" << outputPath
				<< "': " << std::strerror(errno) << "\n";
			return 1;
		}

		std::cout << "path file: " << outputPath << "\n";
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "navigation_load_by_name_demo: " << ex.what() << "\n";
		return 1;
	}
}
