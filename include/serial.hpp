#pragma once

#include <cstdint>
#include <vector>

#include "raw_io.hpp"

enum class ScanMode : uint8_t {
    Horizontal = 0,
    Vertical = 1
};

enum class ModelMode : uint8_t {
    Raw = 0,
    Delta = 1,
    Left2D = 2,
    Top2D = 3,
    Average2D = 4
};

struct SerialOptions {
    ScanMode scan_mode;
    ModelMode model_mode;
};

std::vector<uint8_t> serialize_image(const Image& image, const SerialOptions& options);

Image deserialize_image(const std::vector<uint8_t>& data,
                        uint32_t width,
                        uint32_t height,
                        const SerialOptions& options);