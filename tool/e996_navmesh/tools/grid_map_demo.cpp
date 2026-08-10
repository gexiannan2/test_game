#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

std::uint32_t read_u32_le(const unsigned char* data)
{
	return
		static_cast<std::uint32_t>(data[0]) |
		(static_cast<std::uint32_t>(data[1]) << 8) |
		(static_cast<std::uint32_t>(data[2]) << 16) |
		(static_cast<std::uint32_t>(data[3]) << 24);
}

std::vector<unsigned char> read_file(const std::string& path)
{
	std::ifstream input(path.c_str(), std::ios::binary);
	if (!input)
	{
		throw std::runtime_error("failed to open file");
	}

	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	input.seekg(0, std::ios::beg);

	std::vector<unsigned char> data(static_cast<std::size_t>(size));
	if (!data.empty())
	{
		input.read(reinterpret_cast<char*>(&data[0]), size);
		if (!input)
		{
			throw std::runtime_error("failed to read file");
		}
	}

	return data;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cout << "Usage: " << argv[0] << " <file.map>\n";
		return argc == 1 ? 0 : 1;
	}

	try
	{
		const std::vector<unsigned char> data = read_file(argv[1]);
		if (data.size() < 8)
		{
			throw std::runtime_error("file too small");
		}

		const std::uint32_t width = read_u32_le(&data[0]);
		const std::uint32_t height = read_u32_le(&data[4]);
		const std::size_t cell_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
		const std::size_t expected_size = 8 + cell_count;
		if (data.size() != expected_size)
		{
			throw std::runtime_error("size mismatch for width/height payload");
		}

		std::size_t walkable = 0;
		std::size_t blocked = 0;
		std::vector<std::size_t> hist(256, 0);
		for (std::size_t i = 8; i < data.size(); ++i)
		{
			++hist[data[i]];
			if (data[i] == 0)
			{
				++walkable;
			}
			else
			{
				++blocked;
			}
		}

		std::cout << "File: " << argv[1] << "\n";
		std::cout << "Width: " << width << "\n";
		std::cout << "Height: " << height << "\n";
		std::cout << "Cells: " << cell_count << "\n";
		std::cout << "Walkable(0): " << walkable << "\n";
		std::cout << "Blocked(non-zero): " << blocked << "\n";
		std::cout << "Value histogram:\n";
		for (std::size_t i = 0; i < hist.size(); ++i)
		{
			if (hist[i] == 0)
			{
				continue;
			}
			std::cout << "  0x" << std::hex << std::setw(2) << std::setfill('0') << i
			          << std::dec << ": " << hist[i] << "\n";
		}

		return 0;
	}
	catch (const std::exception& ex)
	{
		std::cerr << "grid_map_demo: " << ex.what() << "\n";
		return 1;
	}
}
