#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "args.hpp"
#include "raw_io.hpp"
#include "serial.hpp"

struct CodecHeader {
    char magic[2];      // "LZ"
    uint8_t width_256;  // width  / 256
    uint8_t height_256; // height / 256
    uint8_t flags;      // compressed + scan + model
};

struct CodecPackage {
    CodecHeader header;
    std::vector<uint8_t> payload;
};

enum HeaderFlags : uint8_t {
    FLAG_COMPRESSED = 1 << 0,
    FLAG_VERTICAL   = 1 << 1,
    FLAG_DELTA      = 1 << 2
};

uint8_t make_flags(const SerialOptions& options, bool compressed);
SerialOptions parse_flags(uint8_t flags);
bool is_compressed(uint8_t flags);

CodecPackage encode_image(const Image& image, bool adaptive_scan, bool use_model);
Image decode_image(const CodecPackage& package);

std::vector<uint8_t> pack_container(const CodecPackage& package);
CodecPackage unpack_container(const std::vector<uint8_t>& bytes);

void compress_file(const ParsedArgs& args);
void decompress_file(const ParsedArgs& args);