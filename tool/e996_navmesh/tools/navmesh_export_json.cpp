#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DetourNavMesh.h"

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

	~LoadedNavMesh()
	{
		if (navMesh)
		{
			dtFreeNavMesh(navMesh);
			navMesh = 0;
		}
	}
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

	return loaded;
}

void writeIndent(std::ofstream& out, int level)
{
	for (int i = 0; i < level; ++i)
	{
		out << "  ";
	}
}

void writeVec3(std::ofstream& out, const float* v)
{
	out << "[" << v[0] << ", " << v[1] << ", " << v[2] << "]";
}

bool exportPolygonsJson(const dtNavMesh& navMesh, const std::string& sourcePath, const std::string& outPath)
{
	std::ofstream out(outPath.c_str());
	if (!out)
	{
		return false;
	}

	out << std::fixed << std::setprecision(6);

	int tileCount = 0;
	int walkablePolyCount = 0;
	for (int i = 0; i < navMesh.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navMesh.getTile(i);
		if (!tile || !tile->header)
		{
			continue;
		}
		++tileCount;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			if (tile->polys[p].getType() != DT_POLYTYPE_OFFMESH_CONNECTION)
			{
				++walkablePolyCount;
			}
		}
	}

	out << "{\n";
	writeIndent(out, 1);
	out << "\"source_navmesh\": \"" << sourcePath << "\",\n";
	writeIndent(out, 1);
	out << "\"tile_count\": " << tileCount << ",\n";
	writeIndent(out, 1);
	out << "\"walkable_polygon_count\": " << walkablePolyCount << ",\n";
	writeIndent(out, 1);
	out << "\"tiles\": [\n";

	bool firstTile = true;
	for (int i = 0; i < navMesh.getMaxTiles(); ++i)
	{
		const dtMeshTile* tile = navMesh.getTile(i);
		if (!tile || !tile->header)
		{
			continue;
		}

		if (!firstTile)
		{
			out << ",\n";
		}
		firstTile = false;

		writeIndent(out, 2);
		out << "{\n";
		writeIndent(out, 3);
		out << "\"tile_index\": " << i << ",\n";
		writeIndent(out, 3);
		out << "\"tile_ref\": " << navMesh.getTileRef(tile) << ",\n";
		writeIndent(out, 3);
		out << "\"poly_count\": " << tile->header->polyCount << ",\n";
		writeIndent(out, 3);
		out << "\"vert_count\": " << tile->header->vertCount << ",\n";
		writeIndent(out, 3);
		out << "\"bmin\": ";
		writeVec3(out, tile->header->bmin);
		out << ",\n";
		writeIndent(out, 3);
		out << "\"bmax\": ";
		writeVec3(out, tile->header->bmax);
		out << ",\n";
		writeIndent(out, 3);
		out << "\"polygons\": [\n";

		bool firstPoly = true;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
			{
				continue;
			}

			float centroid[3] = {0.f, 0.f, 0.f};
			for (unsigned int v = 0; v < poly.vertCount; ++v)
			{
				const float* vert = &tile->verts[poly.verts[v] * 3];
				centroid[0] += vert[0];
				centroid[1] += vert[1];
				centroid[2] += vert[2];
			}
			const float inv = 1.0f / static_cast<float>(poly.vertCount);
			centroid[0] *= inv;
			centroid[1] *= inv;
			centroid[2] *= inv;

			int detailTriCount = 0;
			if (tile->detailMeshes)
			{
				detailTriCount = tile->detailMeshes[p].triCount;
			}

			if (!firstPoly)
			{
				out << ",\n";
			}
			firstPoly = false;

			writeIndent(out, 4);
			out << "{\n";
			writeIndent(out, 5);
			out << "\"poly_index\": " << p << ",\n";
			writeIndent(out, 5);
			out << "\"poly_ref\": "
				<< (navMesh.getPolyRefBase(tile) | static_cast<dtPolyRef>(p)) << ",\n";
			writeIndent(out, 5);
			out << "\"area\": " << static_cast<int>(poly.getArea()) << ",\n";
			writeIndent(out, 5);
			out << "\"flags\": " << poly.flags << ",\n";
			writeIndent(out, 5);
			out << "\"vert_count\": " << static_cast<int>(poly.vertCount) << ",\n";
			writeIndent(out, 5);
			out << "\"detail_tri_count\": " << detailTriCount << ",\n";
			writeIndent(out, 5);
			out << "\"centroid\": ";
			writeVec3(out, centroid);
			out << ",\n";
			writeIndent(out, 5);
			out << "\"vertices\": [\n";

			for (unsigned int v = 0; v < poly.vertCount; ++v)
			{
				const float* vert = &tile->verts[poly.verts[v] * 3];
				writeIndent(out, 6);
				writeVec3(out, vert);
				if (v + 1 != poly.vertCount)
				{
					out << ",";
				}
				out << "\n";
			}

			writeIndent(out, 5);
			out << "]\n";
			writeIndent(out, 4);
			out << "}";
		}

		out << "\n";
		writeIndent(out, 3);
		out << "]\n";
		writeIndent(out, 2);
		out << "}";
	}

	out << "\n";
	writeIndent(out, 1);
	out << "]\n";
	out << "}\n";

	return static_cast<bool>(out);
}

void printUsage(const char* argv0)
{
	std::cout
		<< "Usage: " << argv0 << " <file.navmesh> <output.json>\n"
		<< "Exports the KBEngine .navmesh wrapper into a readable polygon JSON file.\n";
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
		const std::string inputPath = argv[1];
		const std::string outputPath = argv[2];
		LoadedNavMesh loaded = loadWrappedNavMesh(inputPath);
		if (!exportPolygonsJson(*loaded.navMesh, inputPath, outputPath))
		{
			throw std::runtime_error("Failed to write output JSON.");
		}

		std::cout << "Input: " << inputPath << "\n";
		std::cout << "Output: " << outputPath << "\n";
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "navmesh_export_json: " << ex.what() << "\n";
		return 1;
	}
}
