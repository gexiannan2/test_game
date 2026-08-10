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

int align4(int value)
{
	return (value + 3) & ~3;
}

template <typename T>
T readStruct(const unsigned char* data, size_t size, size_t offset, const char* what)
{
	if (offset + sizeof(T) > size)
	{
		std::ostringstream oss;
		oss << "Unexpected EOF while reading " << what
		    << " at offset " << offset;
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

std::string formatMagic(int magic)
{
	std::string label(4, '.');
	label[0] = static_cast<char>((magic >> 24) & 0xff);
	label[1] = static_cast<char>((magic >> 16) & 0xff);
	label[2] = static_cast<char>((magic >> 8) & 0xff);
	label[3] = static_cast<char>(magic & 0xff);
	return label;
}

int computeExpectedTileDataSize(const dtMeshHeader& header)
{
	const int headerSize = align4(static_cast<int>(sizeof(dtMeshHeader)));
	const int vertsSize = align4(static_cast<int>(sizeof(float) * 3 * header.vertCount));
	const int polysSize = align4(static_cast<int>(sizeof(dtPoly) * header.polyCount));
	const int linksSize = align4(static_cast<int>(sizeof(dtLink) * header.maxLinkCount));
	const int detailMeshesSize = align4(static_cast<int>(sizeof(dtPolyDetail) * header.detailMeshCount));
	const int detailVertsSize = align4(static_cast<int>(sizeof(float) * 3 * header.detailVertCount));
	const int detailTrisSize = align4(static_cast<int>(sizeof(unsigned char) * 4 * header.detailTriCount));
	const int bvTreeSize = align4(static_cast<int>(sizeof(dtBVNode) * header.bvNodeCount));
	const int offMeshConsSize = align4(static_cast<int>(sizeof(dtOffMeshConnection) * header.offMeshConCount));

	return headerSize + vertsSize + polysSize + linksSize + detailMeshesSize +
		detailVertsSize + detailTrisSize + bvTreeSize + offMeshConsSize;
}

void printUsage(const char* argv0)
{
	std::cout
		<< "Usage: " << argv0 << " <file.navmesh>\n"
		<< "\n"
		<< "Reads the custom KBEngine .navmesh wrapper and prints the file header\n"
		<< "plus a summary of each embedded Detour tile.\n";
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
		const std::vector<unsigned char> fileData = readFile(path);

		if (fileData.size() < sizeof(NavMeshSetHeader))
		{
			throw std::runtime_error("File is smaller than NavMeshSetHeader.");
		}

		size_t offset = 0;
		const NavMeshSetHeader fileHeader =
			readStruct<NavMeshSetHeader>(&fileData[0], fileData.size(), offset, "NavMeshSetHeader");
		offset += sizeof(NavMeshSetHeader);

		std::cout << "File: " << path << "\n";
		std::cout << "Size: " << fileData.size() << " bytes\n";
		std::cout << "NavMeshSetHeader\n";
		std::cout << "  version: " << fileHeader.version << "\n";
		std::cout << "  tileCount: " << fileHeader.tileCount << "\n";
		std::cout << "  params.orig: " << vec3ToString(fileHeader.params.orig) << "\n";
		std::cout << "  params.tileWidth: " << fileHeader.params.tileWidth << "\n";
		std::cout << "  params.tileHeight: " << fileHeader.params.tileHeight << "\n";
		std::cout << "  params.maxTiles: " << fileHeader.params.maxTiles << "\n";
		std::cout << "  params.maxPolys: " << fileHeader.params.maxPolys << "\n";

		if (fileHeader.tileCount < 0)
		{
			throw std::runtime_error("Invalid tileCount in NavMeshSetHeader.");
		}

		for (int i = 0; i < fileHeader.tileCount; ++i)
		{
			const size_t tileHeaderOffset = offset;
			const NavMeshTileHeader tileHeader =
				readStruct<NavMeshTileHeader>(&fileData[0], fileData.size(), offset, "NavMeshTileHeader");
			offset += sizeof(NavMeshTileHeader);

			if (tileHeader.dataSize < 0)
			{
				std::ostringstream oss;
				oss << "Tile " << i << " has a negative dataSize.";
				throw std::runtime_error(oss.str());
			}

			if (offset + static_cast<size_t>(tileHeader.dataSize) > fileData.size())
			{
				std::ostringstream oss;
				oss << "Tile " << i << " data overruns the file.";
				throw std::runtime_error(oss.str());
			}

			std::cout << "\nTile[" << i << "]\n";
			std::cout << "  fileOffset.tileHeader: " << tileHeaderOffset << "\n";
			std::cout << "  tileRef: " << tileHeader.tileRef << "\n";
			std::cout << "  dataSize: " << tileHeader.dataSize << "\n";

			if (tileHeader.dataSize >= static_cast<int>(sizeof(dtMeshHeader)))
			{
				const dtMeshHeader meshHeader =
					readStruct<dtMeshHeader>(&fileData[0], fileData.size(), offset, "dtMeshHeader");

				const int expectedTileDataSize = computeExpectedTileDataSize(meshHeader);
				std::cout << "  dtMeshHeader.magic: 0x"
				          << std::hex << std::uppercase << meshHeader.magic
				          << std::dec << " ('" << formatMagic(meshHeader.magic) << "')\n";
				std::cout << "  dtMeshHeader.version: " << meshHeader.version << "\n";
				std::cout << "  dtMeshHeader.tile: (" << meshHeader.x
				          << ", " << meshHeader.y
				          << ", layer " << meshHeader.layer << ")\n";
				std::cout << "  dtMeshHeader.userId: " << meshHeader.userId << "\n";
				std::cout << "  dtMeshHeader.polyCount: " << meshHeader.polyCount << "\n";
				std::cout << "  dtMeshHeader.vertCount: " << meshHeader.vertCount << "\n";
				std::cout << "  dtMeshHeader.maxLinkCount: " << meshHeader.maxLinkCount << "\n";
				std::cout << "  dtMeshHeader.detailMeshCount: " << meshHeader.detailMeshCount << "\n";
				std::cout << "  dtMeshHeader.detailVertCount: " << meshHeader.detailVertCount << "\n";
				std::cout << "  dtMeshHeader.detailTriCount: " << meshHeader.detailTriCount << "\n";
				std::cout << "  dtMeshHeader.bvNodeCount: " << meshHeader.bvNodeCount << "\n";
				std::cout << "  dtMeshHeader.offMeshConCount: " << meshHeader.offMeshConCount << "\n";
				std::cout << "  dtMeshHeader.walkableHeight: " << meshHeader.walkableHeight << "\n";
				std::cout << "  dtMeshHeader.walkableRadius: " << meshHeader.walkableRadius << "\n";
				std::cout << "  dtMeshHeader.walkableClimb: " << meshHeader.walkableClimb << "\n";
				std::cout << "  dtMeshHeader.bmin: " << vec3ToString(meshHeader.bmin) << "\n";
				std::cout << "  dtMeshHeader.bmax: " << vec3ToString(meshHeader.bmax) << "\n";
				std::cout << "  expectedDataSizeFromHeader: " << expectedTileDataSize << "\n";
				std::cout << "  dataSizeMatchesHeader: "
				          << (expectedTileDataSize == tileHeader.dataSize ? "yes" : "no") << "\n";
			}
			else
			{
				std::cout << "  dtMeshHeader: unavailable (tile data too small)\n";
			}

			offset += static_cast<size_t>(tileHeader.dataSize);
		}

		const size_t trailingBytes = fileData.size() - offset;
		std::cout << "\nTrailing bytes: " << trailingBytes << "\n";
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "navmesh_dump: " << ex.what() << "\n";
		return 1;
	}
}
