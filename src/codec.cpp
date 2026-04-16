#include "../include/codec.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace {

constexpr char MAGIC_0 = 'L';
constexpr char MAGIC_1 = 'Z';

struct Candidate {
    SerialOptions options;
    bool compressed;
    std::vector<uint8_t> payload;
    size_t total_size;
};

size_t checked_pixel_count(uint32_t width, uint32_t height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height);
}

void validate_image(const Image& image) {
    if (image.width == 0 || image.height == 0) {
        throw std::runtime_error("Image dimensions must be > 0");
    }

    if (image.width % 256 != 0 || image.height % 256 != 0) {
        throw std::runtime_error("Image dimensions must be multiples of 256");
    }

    const size_t expected = checked_pixel_count(image.width, image.height);
    if (image.pixels.size() != expected) {
        throw std::runtime_error("Image pixel buffer size does not match image dimensions");
    }

    if (image.width / 256 > 255 || image.height / 256 > 255) {
        throw std::runtime_error("Image dimensions exceed header capacity");
    }
}

void validate_magic(const CodecHeader& header) {
    if (header.magic[0] != MAGIC_0 || header.magic[1] != MAGIC_1) {
        throw std::runtime_error("Invalid container magic");
    }
}

void validate_flags(uint8_t flags) {
    constexpr uint8_t known_flags = FLAG_COMPRESSED | FLAG_VERTICAL | FLAG_DELTA;
    if ((flags & ~known_flags) != 0) {
        throw std::runtime_error("Unknown flags in container header");
    }
}

CodecHeader make_header(const Image& image, const SerialOptions& options, bool compressed) {
    validate_image(image);

    CodecHeader header{};
    header.magic[0] = MAGIC_0;
    header.magic[1] = MAGIC_1;
    header.width_256 = static_cast<uint8_t>(image.width / 256);
    header.height_256 = static_cast<uint8_t>(image.height / 256);
    header.flags = make_flags(options, compressed);
    return header;
}

uint32_t restore_width(const CodecHeader& header) {
    return static_cast<uint32_t>(header.width_256) * 256u;
}

uint32_t restore_height(const CodecHeader& header) {
    return static_cast<uint32_t>(header.height_256) * 256u;
}

// -----------------------------------------------------------------------------
// Temporary compressor: byte-oriented RLE
// Format: [marker=0xFF][count][value] for runs of length >= 4 or value == 0xFF
// Otherwise literal bytes are copied as-is.
// -----------------------------------------------------------------------------

std::vector<uint8_t> compress_payload(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    out.reserve(input.size());

    size_t i = 0;
    while (i < input.size()) {
        constexpr uint8_t marker = 0xFF;
        const uint8_t value = input[i];
        size_t run = 1;

        while (i + run < input.size() &&
               input[i + run] == value &&
               run < 255) {
            ++run;
        }

        if (run >= 4 || value == marker) {
            out.push_back(marker);
            out.push_back(static_cast<uint8_t>(run));
            out.push_back(value);
            i += run;
        } else {
            out.push_back(value);
            ++i;
        }
    }

    return out;
}

std::vector<uint8_t> decompress_payload(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    constexpr uint8_t marker = 0xFF;

    size_t i = 0;
    while (i < input.size()) {
        if (input[i] != marker) {
            out.push_back(input[i]);
            ++i;
            continue;
        }

        if (i + 2 >= input.size()) {
            throw std::runtime_error("Corrupted compressed payload");
        }

        const uint8_t count = input[i + 1];
        const uint8_t value = input[i + 2];

        if (count == 0) {
            throw std::runtime_error("Invalid RLE count in compressed payload");
        }

        out.insert(out.end(), count, value);
        i += 3;
    }

    return out;
}

Candidate build_candidate(const Image& image, const SerialOptions& options) {
    const std::vector<uint8_t> serialized = serialize_image(image, options);
    const std::vector<uint8_t> compressed = compress_payload(serialized);

    Candidate candidate_raw{
        options,
        false,
        serialized,
        5 + serialized.size()
    };

    Candidate candidate_compressed{
        options,
        true,
        compressed,
        5 + compressed.size()
    };

    // According to the spec, if compression is not beneficial, keep original form.
    return (candidate_compressed.total_size < candidate_raw.total_size)
        ? std::move(candidate_compressed)
        : std::move(candidate_raw);
}

std::vector<uint8_t> read_entire_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("Cannot open input file: " + path);
    }

    const std::streamsize size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("Failed to determine input file size: " + path);
    }

    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Failed to read input file: " + path);
    }

    return bytes;
}

void write_entire_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Cannot open output file: " + path);
    }

    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            throw std::runtime_error("Failed to write output file: " + path);
        }
    }
}

} // namespace

uint8_t make_flags(const SerialOptions& options, bool compressed) {
    uint8_t flags = 0;

    if (compressed) {
        flags |= FLAG_COMPRESSED;
    }

    if (options.scan_mode == ScanMode::Vertical) {
        flags |= FLAG_VERTICAL;
    }

    if (options.model_mode == ModelMode::Delta) {
        flags |= FLAG_DELTA;
    }

    return flags;
}

SerialOptions parse_flags(uint8_t flags) {
    validate_flags(flags);

    SerialOptions options{};
    options.scan_mode = (flags & FLAG_VERTICAL) ? ScanMode::Vertical
                                                : ScanMode::Horizontal;
    options.model_mode = (flags & FLAG_DELTA) ? ModelMode::Delta
                                              : ModelMode::Raw;
    return options;
}

bool is_compressed(uint8_t flags) {
    validate_flags(flags);
    return (flags & FLAG_COMPRESSED) != 0;
}

CodecPackage encode_image(const Image& image, bool adaptive_scan, bool use_model) {
    validate_image(image);

    std::vector<ScanMode> scan_modes{ScanMode::Horizontal};
    if (adaptive_scan) {
        scan_modes.push_back(ScanMode::Vertical);
    }

    std::vector<ModelMode> model_modes{ModelMode::Raw};
    if (use_model) {
        model_modes.push_back(ModelMode::Delta);
    }

    bool have_best = false;
    Candidate best{};

    for (const ScanMode scan_mode : scan_modes) {
        for (const ModelMode model_mode : model_modes) {
            const SerialOptions options{scan_mode, model_mode};
            Candidate candidate = build_candidate(image, options);

            if (!have_best || candidate.total_size < best.total_size) {
                best = std::move(candidate);
                have_best = true;
            }
        }
    }

    if (!have_best) {
        throw std::runtime_error("Failed to build any encoding candidate");
    }

    return CodecPackage{
        make_header(image, best.options, best.compressed),
        std::move(best.payload)
    };
}

Image decode_image(const CodecPackage& package) {
    validate_magic(package.header);
    validate_flags(package.header.flags);

    const uint32_t width = restore_width(package.header);
    const uint32_t height = restore_height(package.header);

    if (width == 0 || height == 0) {
        throw std::runtime_error("Invalid image dimensions in container");
    }

    std::vector<uint8_t> serialized;

    if (is_compressed(package.header.flags)) {
        serialized = decompress_payload(package.payload);
    } else {
        serialized = package.payload;
    }

    const size_t expected = checked_pixel_count(width, height);
    if (serialized.size() != expected) {
        throw std::runtime_error("Decoded payload size does not match image dimensions");
    }

    const SerialOptions options = parse_flags(package.header.flags);
    return deserialize_image(serialized, width, height, options);
}

std::vector<uint8_t> pack_container(const CodecPackage& package) {
    validate_magic(package.header);
    validate_flags(package.header.flags);

    std::vector<uint8_t> out;
    out.reserve(5 + package.payload.size());

    out.push_back(static_cast<uint8_t>(package.header.magic[0]));
    out.push_back(static_cast<uint8_t>(package.header.magic[1]));
    out.push_back(package.header.width_256);
    out.push_back(package.header.height_256);
    out.push_back(package.header.flags);

    out.insert(out.end(), package.payload.begin(), package.payload.end());
    return out;
}

CodecPackage unpack_container(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 5) {
        throw std::runtime_error("Input is too small to be a valid container");
    }

    CodecHeader header{};
    header.magic[0] = static_cast<char>(bytes[0]);
    header.magic[1] = static_cast<char>(bytes[1]);
    header.width_256 = bytes[2];
    header.height_256 = bytes[3];
    header.flags = bytes[4];

    validate_magic(header);
    validate_flags(header.flags);

    std::vector payload(bytes.begin() + 5, bytes.end());
    return CodecPackage{header, std::move(payload)};
}

void compress_file(const ParsedArgs& args) {
    Image image = read_raw_image(args.infile, args.width);
    CodecPackage package = encode_image(image, args.adaptive_scan, args.use_model);
    std::vector<uint8_t> bytes = pack_container(package);
    write_entire_file(args.outfile, bytes);
}

void decompress_file(const ParsedArgs& args) {
    const std::vector<uint8_t> bytes = read_entire_file(args.infile);
    CodecPackage package = unpack_container(bytes);
    Image image = decode_image(package);
    write_raw_image(args.outfile, image);
}