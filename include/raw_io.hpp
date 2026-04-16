#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Image {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> pixels;
};

Image read_raw_image(const std::string& path, uint32_t width);
void write_raw_image(const std::string& path, const Image& image);