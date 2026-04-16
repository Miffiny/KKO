#include "../include/raw_io.hpp"

#include <fstream>
#include <stdexcept>

Image read_raw_image(const std::string& path, uint32_t width) {
    if (width == 0) {
        throw std::runtime_error("Image width must be > 0");
    }

    if (width % 256 != 0) {
        throw std::runtime_error("Image width must be a multiple of 256");
    }

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("Cannot open input file: " + path);
    }

    std::streamsize file_size = in.tellg();
    if (file_size < 0) {
        throw std::runtime_error("Failed to determine file size: " + path);
    }

    if (file_size % static_cast<std::streamsize>(width) != 0) {
        throw std::runtime_error("RAW file size is not divisible by image width");
    }

    const auto height = static_cast<uint32_t>(file_size / static_cast<std::streamsize>(width));

    if (height == 0) {
        throw std::runtime_error("Computed image height is 0");
    }

    if (height % 256 != 0) {
        throw std::runtime_error("Image height must be a multiple of 256");
    }

    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> pixels(static_cast<size_t>(file_size));
    if (!in.read(reinterpret_cast<char*>(pixels.data()), file_size)) {
        throw std::runtime_error("Failed to read RAW file: " + path);
    }

    return Image{width, height, std::move(pixels)};
}

void write_raw_image(const std::string& path, const Image& image) {
    if (image.width == 0 || image.height == 0) {
        throw std::runtime_error("Image dimensions must be > 0");
    }

    if (image.width % 256 != 0 || image.height % 256 != 0) {
        throw std::runtime_error("Image dimensions must be multiples of 256");
    }

    const size_t expected_size =
        static_cast<size_t>(image.width) * static_cast<size_t>(image.height);

    if (image.pixels.size() != expected_size) {
        throw std::runtime_error("Pixel buffer size does not match image dimensions");
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Cannot open output file: " + path);
    }

    if (!image.pixels.empty()) {
        out.write(reinterpret_cast<const char*>(image.pixels.data()),
                  static_cast<std::streamsize>(image.pixels.size()));

        if (!out) {
            throw std::runtime_error("Failed to write RAW file: " + path);
        }
    }
}