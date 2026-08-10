#include <cerrno>
#include <cstring>
#include <fstream>
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

const float* getDetailVertex(const dtMeshTile& tile, const dtPoly& poly, const dtPolyDetail& detail, unsigned char vertexIndex)
{
	if (vertexIndex < poly.vertCount)
	{
		return &tile.verts[poly.verts[vertexIndex] * 3];
	}

	return &tile.detailVerts[(detail.vertBase + (vertexIndex - poly.vertCount)) * 3];
}

void printUsage(const char* argv0)
{
	std::cout
		<< "Usage: " << argv0 << " <input.navmesh> <output.obj>\n"
		<< "Exports baked navmesh detail triangles to OBJ for external viewing.\n";
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
		const std::string navmeshPath = argv[1];
		const std::string objPath = argv[2];
		LoadedNavMesh loaded = loadWrappedNavMesh(navmeshPath);

		std::ofstream out(objPath.c_str());
		if (!out)
		{
			std::ostringstream oss;
			oss << "Failed to open OBJ output '" << objPath << "': " << std::strerror(errno);
			throw std::runtime_error(oss.str());
		}

		out << "# Exported from " << navmeshPath << "\n";

		int triangleCount = 0;
		int vertexCount = 0;
		int faceBase = 1;
		const dtNavMesh* navMesh = loaded.navMesh;

		for (int tileIndex = 0; tileIndex < navMesh->getMaxTiles(); ++tileIndex)
		{
			const dtMeshTile* tile = navMesh->getTile(tileIndex);
			if (!tile || !tile->header)
			{
				continue;
			}

			for (int polyIndex = 0; polyIndex < tile->header->polyCount; ++polyIndex)
			{
				const dtPoly& poly = tile->polys[polyIndex];
				if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
				{
					continue;
				}

				const dtPolyDetail& detail = tile->detailMeshes[polyIndex];
				for (unsigned int triOffset = 0; triOffset < detail.triCount; ++triOffset)
				{
					const unsigned char* tri = &tile->detailTris[(detail.triBase + triOffset) * 4];
					const float* v0 = getDetailVertex(*tile, poly, detail, tri[0]);
					const float* v1 = getDetailVertex(*tile, poly, detail, tri[1]);
					const float* v2 = getDetailVertex(*tile, poly, detail, tri[2]);

					out << "v " << v0[0] << " " << v0[1] << " " << v0[2] << "\n";
					out << "v " << v1[0] << " " << v1[1] << " " << v1[2] << "\n";
					out << "v " << v2[0] << " " << v2[1] << " " << v2[2] << "\n";
					out << "f " << faceBase << " " << (faceBase + 1) << " " << (faceBase + 2) << "\n";

					faceBase += 3;
					vertexCount += 3;
					++triangleCount;
				}
			}
		}

		std::cout << "Input navmesh: " << navmeshPath << "\n";
		std::cout << "Output obj: " << objPath << "\n";
		std::cout << "Exported vertices: " << vertexCount << "\n";
		std::cout << "Exported triangles: " << triangleCount << "\n";
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "navmesh_export_obj: " << ex.what() << "\n";
		return 1;
	}
}
