#include "../include/codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr char MAGIC_0 = 'L';
constexpr char MAGIC_1 = 'Z';

constexpr size_t CONTAINER_HEADER_SIZE = 5;

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

size_t checked_pixel_count(const uint32_t width, const uint32_t height) {
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

uint32_t restore_width(const CodecHeader& header) {
    return static_cast<uint32_t>(header.width_256) * 256u;
}

uint32_t restore_height(const CodecHeader& header) {
    return static_cast<uint32_t>(header.height_256) * 256u;
}

void write_u16_le(std::vector<uint8_t>& out, const uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void write_u32_le(std::vector<uint8_t>& out, const uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

uint16_t read_u16_le(const std::vector<uint8_t>& in, size_t& pos) {
    if (pos + 2 > in.size()) {
        throw std::runtime_error("Unexpected end of payload while reading uint16");
    }

    const uint16_t value =
        static_cast<uint16_t>(in[pos]) |
        (static_cast<uint16_t>(in[pos + 1]) << 8);

    pos += 2;
    return value;
}

uint32_t read_u32_le(const std::vector<uint8_t>& in, size_t& pos) {
    if (pos + 4 > in.size()) {
        throw std::runtime_error("Unexpected end of payload while reading uint32");
    }

    const uint32_t value =
        static_cast<uint32_t>(in[pos]) |
        (static_cast<uint32_t>(in[pos + 1]) << 8) |
        (static_cast<uint32_t>(in[pos + 2]) << 16) |
        (static_cast<uint32_t>(in[pos + 3]) << 24);

    pos += 4;
    return value;
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

// -----------------------------------------------------------------------------
// Header / flags
// -----------------------------------------------------------------------------

void validate_flags(const uint8_t flags) {
    constexpr uint8_t known_bits = FLAG_COMPRESSED | FLAG_VERTICAL | MODEL_MASK;
    if ((flags & ~known_bits) != 0) {
        throw std::runtime_error("Unknown flags in container header");
    }

    const uint8_t model_bits = (flags & MODEL_MASK) >> MODEL_SHIFT;
    if (model_bits > static_cast<uint8_t>(ModelMode::Paeth2D)) {
        throw std::runtime_error("Unknown model encoded in container header");
    }
}

CodecHeader make_header(const Image& image, const SerialOptions& options, const bool compressed) {
    validate_image(image);

    CodecHeader header{};
    header.magic[0] = MAGIC_0;
    header.magic[1] = MAGIC_1;
    header.width_256 = static_cast<uint8_t>(image.width / 256);
    header.height_256 = static_cast<uint8_t>(image.height / 256);
    header.flags = make_flags(options, compressed);
    return header;
}

// -----------------------------------------------------------------------------
// LZSS
// -----------------------------------------------------------------------------

// Token stream format:
//   control byte per group of up to 8 tokens
//   control bit 1 => literal byte follows
//   control bit 0 => match follows as 2 bytes
//
// Match encoding (2 bytes):
//   offset: 12 bits, stored as distance-1 in [0..4095]
//   length: 4 bits, stored as length-3 in [0..15] => actual length [3..18]
//
// This is intentionally simple and compact.

constexpr int LZ_WINDOW_SIZE = 4096;
constexpr int LZ_MIN_MATCH   = 3;
constexpr int LZ_MAX_MATCH   = 18;

std::vector<uint8_t> lzss_compress(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    if (input.empty()) {
        return out;
    }

    size_t pos = 0;
    while (pos < input.size()) {
        const size_t control_pos = out.size();
        out.push_back(0);
        uint8_t control = 0;

        for (int bit = 0; bit < 8 && pos < input.size(); ++bit) {
            int best_len = 0;
            int best_dist = 0;

            const int window_start =
                static_cast<int>(pos) > LZ_WINDOW_SIZE
                    ? static_cast<int>(pos) - LZ_WINDOW_SIZE
                    : 0;

            for (int candidate = window_start; candidate < static_cast<int>(pos); ++candidate) {
                int len = 0;
                while (len < LZ_MAX_MATCH &&
                       pos + static_cast<size_t>(len) < input.size() &&
                       input[static_cast<size_t>(candidate + len)] == input[pos + static_cast<size_t>(len)]) {
                    ++len;
                }

                if (len >= LZ_MIN_MATCH && len > best_len) {
                    best_len = len;
                    best_dist = static_cast<int>(pos) - candidate;

                    if (best_len == LZ_MAX_MATCH) {
                        break;
                    }
                }
            }

            if (best_len >= LZ_MIN_MATCH) {
                const auto dist_code = static_cast<uint16_t>(best_dist - 1);
                const auto len_code = static_cast<uint8_t>(best_len - LZ_MIN_MATCH);

                const auto b1 = static_cast<uint8_t>(dist_code & 0xFFu);
                const auto b2 = static_cast<uint8_t>(((dist_code >> 8) & 0x0Fu) << 4 |
                                                        (len_code & 0x0Fu));

                out.push_back(b1);
                out.push_back(b2);
                pos += static_cast<size_t>(best_len);
            } else {
                control |= static_cast<uint8_t>(1u << bit);
                out.push_back(input[pos]);
                ++pos;
            }
        }

        out[control_pos] = control;
    }

    return out;
}

std::vector<uint8_t> lzss_decompress(const std::vector<uint8_t>& input,
                                     const size_t expected_output_size) {
    std::vector<uint8_t> out;
    out.reserve(expected_output_size);

    size_t pos = 0;
    while (pos < input.size() && out.size() < expected_output_size) {
        const uint8_t control = input[pos++];
        for (int bit = 0; bit < 8 && out.size() < expected_output_size; ++bit) {
            if (pos > input.size()) {
                throw std::runtime_error("Corrupted LZSS stream");
            }

            const bool literal = (control & (1u << bit)) != 0;
            if (literal) {
                if (pos >= input.size()) {
                    throw std::runtime_error("Corrupted LZSS literal");
                }
                out.push_back(input[pos++]);
            } else {
                if (pos + 1 >= input.size()) {
                    throw std::runtime_error("Corrupted LZSS match");
                }

                const uint8_t b1 = input[pos++];
                const uint8_t b2 = input[pos++];

                const uint16_t dist_code =
                    static_cast<uint16_t>(b1) |
                    (static_cast<uint16_t>((b2 >> 4) & 0x0F) << 8);
                const int distance = static_cast<int>(dist_code) + 1;
                const int length = static_cast<int>(b2 & 0x0F) + LZ_MIN_MATCH;

                if (distance <= 0 || static_cast<size_t>(distance) > out.size()) {
                    throw std::runtime_error("Invalid LZSS back-reference");
                }

                const size_t start = out.size() - static_cast<size_t>(distance);
                for (int i = 0; i < length && out.size() < expected_output_size; ++i) {
                    out.push_back(out[start + static_cast<size_t>(i)]);
                }
            }
        }
    }

    if (out.size() != expected_output_size) {
        throw std::runtime_error("LZSS output size mismatch");
    }

    return out;
}

// -----------------------------------------------------------------------------
// Huffman
// -----------------------------------------------------------------------------

struct HuffmanNode {
    uint64_t freq = 0;
    int symbol = -1;
    int left = -1;
    int right = -1;
};

struct HuffmanCode {
    uint32_t bits = 0;
    uint8_t length = 0;
};

class BitWriter {
public:
    void write_bit(const uint8_t bit) {
        current_byte_ = static_cast<uint8_t>((current_byte_ << 1) | (bit & 1u));
        ++bit_count_;

        if (bit_count_ == 8) {
            bytes_.push_back(current_byte_);
            current_byte_ = 0;
            bit_count_ = 0;
        }
    }

    void write_bits_msb(const uint32_t bits, const uint8_t count) {
        for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
            write_bit(static_cast<uint8_t>((bits >> i) & 1u));
        }
    }

    std::vector<uint8_t> finish() {
        if (bit_count_ != 0) {
            current_byte_ <<= static_cast<uint8_t>(8 - bit_count_);
            bytes_.push_back(current_byte_);
            current_byte_ = 0;
            bit_count_ = 0;
        }
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t current_byte_ = 0;
    uint8_t bit_count_ = 0;
};

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

    bool read_bit(uint8_t& bit) {
        if (byte_pos_ >= bytes_.size()) {
            return false;
        }

        bit = static_cast<uint8_t>((bytes_[byte_pos_] >> (7 - bit_pos_)) & 1u);
        ++bit_pos_;

        if (bit_pos_ == 8) {
            bit_pos_ = 0;
            ++byte_pos_;
        }

        return true;
    }

private:
    const std::vector<uint8_t>& bytes_;
    size_t byte_pos_ = 0;
    uint8_t bit_pos_ = 0;
};

void build_code_lengths(const std::vector<HuffmanNode>& nodes,
                        const int node_idx,
                        const int depth,
                        std::array<uint8_t, 256>& lengths) {
    const HuffmanNode& node = nodes[static_cast<size_t>(node_idx)];

    if (node.symbol >= 0) {
        lengths[static_cast<size_t>(node.symbol)] =
            static_cast<uint8_t>(depth == 0 ? 1 : depth);
        return;
    }

    build_code_lengths(nodes, node.left, depth + 1, lengths);
    build_code_lengths(nodes, node.right, depth + 1, lengths);
}

std::array<uint8_t, 256> make_huffman_code_lengths(const std::vector<uint8_t>& input) {
    std::array<uint64_t, 256> freq{};
    for (const uint8_t byte : input) {
        ++freq[byte];
    }

    struct QueueItem {
        uint64_t freq;
        int node_idx;
        bool operator>(const QueueItem& other) const {
            if (freq != other.freq) return freq > other.freq;
            return node_idx > other.node_idx;
        }
    };

    std::vector<HuffmanNode> nodes;
    nodes.reserve(512);

    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> pq;

    for (int sym = 0; sym < 256; ++sym) {
        if (freq[static_cast<size_t>(sym)] > 0) {
            nodes.push_back(HuffmanNode{freq[static_cast<size_t>(sym)], sym, -1, -1});
            pq.push(QueueItem{freq[static_cast<size_t>(sym)], static_cast<int>(nodes.size() - 1)});
        }
    }

    std::array<uint8_t, 256> lengths{};
    if (pq.empty()) {
        return lengths;
    }

    while (pq.size() > 1) {
        const QueueItem a = pq.top();
        pq.pop();
        const QueueItem b = pq.top();
        pq.pop();

        nodes.push_back(HuffmanNode{
            a.freq + b.freq,
            -1,
            a.node_idx,
            b.node_idx
        });

        pq.push(QueueItem{
            a.freq + b.freq,
            static_cast<int>(nodes.size() - 1)
        });
    }

    build_code_lengths(nodes, pq.top().node_idx, 0, lengths);
    return lengths;
}

std::array<HuffmanCode, 256> make_canonical_codes(const std::array<uint8_t, 256>& lengths) {
    std::array<int, 33> bl_count{};
    for (const uint8_t len : lengths) {
        if (len > 32) {
            throw std::runtime_error("Huffman code length exceeds supported maximum");
        }
        if (len > 0) {
            ++bl_count[len];
        }
    }

    std::array<uint32_t, 33> next_code{};
    uint32_t code = 0;
    for (int bits = 1; bits <= 32; ++bits) {
        code = (code + static_cast<uint32_t>(bl_count[bits - 1])) << 1;
        next_code[bits] = code;
    }

    std::array<HuffmanCode, 256> codes{};
    for (int sym = 0; sym < 256; ++sym) {
        const uint8_t len = lengths[static_cast<size_t>(sym)];
        if (len > 0) {
            codes[static_cast<size_t>(sym)] = HuffmanCode{next_code[len], len};
            ++next_code[len];
        }
    }

    return codes;
}

std::vector<uint8_t> huffman_compress(const std::vector<uint8_t>& input) {
    const auto lengths = make_huffman_code_lengths(input);
    const auto codes = make_canonical_codes(lengths);

    std::vector<std::pair<uint8_t, uint8_t>> used;
    used.reserve(256);
    for (int sym = 0; sym < 256; ++sym) {
        const uint8_t len = lengths[static_cast<size_t>(sym)];
        if (len > 0) {
            used.push_back({static_cast<uint8_t>(sym), len});
        }
    }

    std::vector<uint8_t> out;
    write_u16_le(out, static_cast<uint16_t>(used.size()));
    for (const auto& [sym, len] : used) {
        out.push_back(sym);
        out.push_back(len);
    }

    BitWriter bw;
    for (const uint8_t byte : input) {
        const HuffmanCode hc = codes[byte];
        bw.write_bits_msb(hc.bits, hc.length);
    }

    std::vector<uint8_t> bitstream = bw.finish();
    out.insert(out.end(), bitstream.begin(), bitstream.end());
    return out;
}

std::vector<uint8_t> huffman_decompress(const std::vector<uint8_t>& payload,
                                        size_t& pos,
                                        const size_t expected_output_size) {
    std::array<uint8_t, 256> lengths{};

    const uint16_t used_count = read_u16_le(payload, pos);
    for (uint16_t i = 0; i < used_count; ++i) {
        if (pos + 2 > payload.size()) {
            throw std::runtime_error("Corrupted Huffman header");
        }

        const uint8_t sym = payload[pos++];
        const uint8_t len = payload[pos++];

        if (len == 0 || len > 32) {
            throw std::runtime_error("Invalid Huffman code length");
        }

        lengths[sym] = len;
    }

    const auto codes = make_canonical_codes(lengths);

    struct DecodeNode {
        int child[2] = {-1, -1};
        int symbol = -1;
    };

    std::vector<DecodeNode> tree(1);

    for (int sym = 0; sym < 256; ++sym) {
        const HuffmanCode hc = codes[static_cast<size_t>(sym)];
        if (hc.length == 0) {
            continue;
        }

        int node = 0;
        for (int bit_index = static_cast<int>(hc.length) - 1; bit_index >= 0; --bit_index) {
            const int bit = static_cast<int>((hc.bits >> bit_index) & 1u);

            if (tree[static_cast<size_t>(node)].child[bit] == -1) {
                tree[static_cast<size_t>(node)].child[bit] = static_cast<int>(tree.size());
                tree.push_back(DecodeNode{});
            }

            node = tree[static_cast<size_t>(node)].child[bit];
        }

        tree[static_cast<size_t>(node)].symbol = sym;
    }

    std::vector<uint8_t> bitstream(payload.begin() + static_cast<std::ptrdiff_t>(pos), payload.end());
    BitReader br(bitstream);

    std::vector<uint8_t> out;
    out.reserve(expected_output_size);

    while (out.size() < expected_output_size) {
        int node = 0;

        while (tree[static_cast<size_t>(node)].symbol < 0) {
            uint8_t bit = 0;
            if (!br.read_bit(bit)) {
                throw std::runtime_error("Unexpected end of Huffman bitstream");
            }

            node = tree[static_cast<size_t>(node)].child[bit];
            if (node < 0) {
                throw std::runtime_error("Invalid Huffman code in bitstream");
            }
        }

        out.push_back(static_cast<uint8_t>(tree[static_cast<size_t>(node)].symbol));
    }

    pos = payload.size();
    return out;
}

// -----------------------------------------------------------------------------
// Combined payload codec: LZSS -> Huffman
//
// Payload format for compressed candidate:
//   [u32 lz_size]
//   [u16 used_symbol_count]
//   repeated used_symbol_count times:
//       [u8 symbol][u8 bit_length]
//   [huffman_bitstream...]
//
// Decompression:
//   payload -> Huffman decode to lz_size bytes -> LZSS decode to raw serialized
// -----------------------------------------------------------------------------

std::vector<uint8_t> compress_payload(const std::vector<uint8_t>& input) {
    const std::vector<uint8_t> lz_stream = lzss_compress(input);
    std::vector<uint8_t> out;
    write_u32_le(out, static_cast<uint32_t>(lz_stream.size()));

    const std::vector<uint8_t> huff = huffman_compress(lz_stream);
    out.insert(out.end(), huff.begin(), huff.end());
    return out;
}

std::vector<uint8_t> decompress_payload(const std::vector<uint8_t>& input,
                                        const size_t expected_output_size) {
    size_t pos = 0;
    const uint32_t lz_size = read_u32_le(input, pos);

    const std::vector<uint8_t> lz_stream =
        huffman_decompress(input, pos, static_cast<size_t>(lz_size));

    return lzss_decompress(lz_stream, expected_output_size);
}

// -----------------------------------------------------------------------------
// Candidate search
// -----------------------------------------------------------------------------

struct Candidate {
    SerialOptions options{};
    bool compressed = false;
    std::vector<uint8_t> payload;
    size_t total_size = 0;
};

Candidate build_candidate(const Image& image, const SerialOptions& options) {
    const std::vector<uint8_t> serialized = serialize_image(image, options);
    const std::vector<uint8_t> compressed = compress_payload(serialized);

    Candidate raw_candidate{
        options,
        false,
        serialized,
        CONTAINER_HEADER_SIZE + serialized.size()
    };

    Candidate compressed_candidate{
        options,
        true,
        compressed,
        CONTAINER_HEADER_SIZE + compressed.size()
    };

    return (compressed_candidate.total_size < raw_candidate.total_size)
        ? std::move(compressed_candidate)
        : std::move(raw_candidate);
}

std::vector<ModelMode> allowed_model_modes(const bool use_model) {
    if (!use_model) {
        return {ModelMode::Raw};
    }
    return {
        ModelMode::Raw,
        ModelMode::Delta,
        ModelMode::Left2D,
        ModelMode::Top2D,
        ModelMode::Average2D,
        ModelMode::Paeth2D
    };
}

std::vector<ScanMode> allowed_scan_modes(const bool adaptive_scan) {
    if (!adaptive_scan) {
        return {ScanMode::Horizontal};
    }

    return {ScanMode::Horizontal, ScanMode::Vertical};
}

} // namespace

uint8_t make_flags(const SerialOptions& options, const bool compressed) {
    uint8_t flags = 0;

    if (compressed) {
        flags |= FLAG_COMPRESSED;
    }

    if (options.scan_mode == ScanMode::Vertical) {
        flags |= FLAG_VERTICAL;
    }

    const uint8_t model_bits =
        static_cast<uint8_t>(options.model_mode) << MODEL_SHIFT;

    if ((model_bits & ~MODEL_MASK) != 0) {
        throw std::runtime_error("Model mode does not fit into header flags");
    }

    flags |= model_bits;
    return flags;
}

SerialOptions parse_flags(const uint8_t flags) {
    validate_flags(flags);

    SerialOptions options{};
    options.scan_mode = (flags & FLAG_VERTICAL) ? ScanMode::Vertical
                                                : ScanMode::Horizontal;

    const uint8_t model_bits = static_cast<uint8_t>((flags & MODEL_MASK) >> MODEL_SHIFT);
    options.model_mode = static_cast<ModelMode>(model_bits);
    return options;
}

bool is_compressed(const uint8_t flags) {
    validate_flags(flags);
    return (flags & FLAG_COMPRESSED) != 0;
}

CodecPackage encode_image(const Image& image, const bool adaptive_scan, const bool use_model) {
    validate_image(image);

    const std::vector<ScanMode> scan_modes = allowed_scan_modes(adaptive_scan);
    const std::vector<ModelMode> model_modes = allowed_model_modes(use_model);

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
        throw std::runtime_error("Failed to build encoding candidates");
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
    const size_t expected_serialized_size = checked_pixel_count(width, height);

    std::vector<uint8_t> serialized;

    if (is_compressed(package.header.flags)) {
        serialized = decompress_payload(package.payload, expected_serialized_size);
    } else {
        serialized = package.payload;
    }

    if (serialized.size() != expected_serialized_size) {
        throw std::runtime_error("Decoded payload size does not match image dimensions");
    }

    const SerialOptions options = parse_flags(package.header.flags);
    return deserialize_image(serialized, width, height, options);
}

std::vector<uint8_t> pack_container(const CodecPackage& package) {
    validate_magic(package.header);
    validate_flags(package.header.flags);

    std::vector<uint8_t> out;
    out.reserve(CONTAINER_HEADER_SIZE + package.payload.size());

    out.push_back(static_cast<uint8_t>(package.header.magic[0]));
    out.push_back(static_cast<uint8_t>(package.header.magic[1]));
    out.push_back(package.header.width_256);
    out.push_back(package.header.height_256);
    out.push_back(package.header.flags);

    out.insert(out.end(), package.payload.begin(), package.payload.end());
    return out;
}

CodecPackage unpack_container(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < CONTAINER_HEADER_SIZE) {
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

    std::vector<uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(CONTAINER_HEADER_SIZE),
                                 bytes.end());

    return CodecPackage{header, std::move(payload)};
}

void compress_file(const ParsedArgs& args) {
    const Image image = read_raw_image(args.infile, args.width);
    const CodecPackage package = encode_image(image, args.adaptive_scan, args.use_model);
    const std::vector<uint8_t> bytes = pack_container(package);
    write_entire_file(args.outfile, bytes);
}

void decompress_file(const ParsedArgs& args) {
    const std::vector<uint8_t> bytes = read_entire_file(args.infile);
    const CodecPackage package = unpack_container(bytes);
    const Image image = decode_image(package);
    write_raw_image(args.outfile, image);
}