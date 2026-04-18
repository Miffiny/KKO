#include "../include/codec.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr char MAGIC_0 = 'L';
constexpr char MAGIC_1 = 'Z';
constexpr size_t HEADER_SIZE = 5;

constexpr int LZ_WINDOW_SIZE = 1024;
constexpr int LZ_MIN_MATCH = 3;
constexpr int LZ_MAX_MATCH = 18;

size_t pixel_count(uint32_t w, uint32_t h) { return static_cast<size_t>(w) * h; }

void validate_image(const Image& image) {
    if (!image.width || !image.height) throw std::runtime_error("Image dimensions must be > 0");
    if (image.width % 256 || image.height % 256) throw std::runtime_error("Image dimensions must be multiples of 256");
    if (image.pixels.size() != pixel_count(image.width, image.height))
        throw std::runtime_error("Image pixel buffer size does not match image dimensions");
    if (image.width / 256 > 255 || image.height / 256 > 255)
        throw std::runtime_error("Image dimensions exceed header capacity");
}

void validate_magic(const CodecHeader& h) {
    if (h.magic[0] != MAGIC_0 || h.magic[1] != MAGIC_1) throw std::runtime_error("Invalid container magic");
}

void write_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void write_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

uint16_t read_u16_le(const std::vector<uint8_t>& in, size_t& pos) {
    if (pos + 2 > in.size()) throw std::runtime_error("Unexpected end of payload while reading uint16");
    uint16_t v = static_cast<uint16_t>(in[pos]) | (static_cast<uint16_t>(in[pos + 1]) << 8);
    pos += 2;
    return v;
}

uint32_t read_u32_le(const std::vector<uint8_t>& in, size_t& pos) {
    if (pos + 4 > in.size()) throw std::runtime_error("Unexpected end of payload while reading uint32");
    uint32_t v = static_cast<uint32_t>(in[pos])
               | (static_cast<uint32_t>(in[pos + 1]) << 8)
               | (static_cast<uint32_t>(in[pos + 2]) << 16)
               | (static_cast<uint32_t>(in[pos + 3]) << 24);
    pos += 4;
    return v;
}

std::vector<uint8_t> read_entire_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("Cannot open input file: " + path);
    std::streamsize size = in.tellg();
    if (size < 0) throw std::runtime_error("Failed to determine input file size: " + path);
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Failed to read input file: " + path);
    return bytes;
}

void write_entire_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output file: " + path);
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("Failed to write output file: " + path);
    }
}

void validate_flags(uint8_t flags) {
    constexpr uint8_t known = FLAG_COMPRESSED | FLAG_VERTICAL | MODEL_MASK;
    if ((flags & ~known) != 0) throw std::runtime_error("Unknown flags in container header");
    const uint8_t model = (flags & MODEL_MASK) >> MODEL_SHIFT;
    if (model > static_cast<uint8_t>(ModelMode::Paeth2D))
        throw std::runtime_error("Unknown model encoded in container header");
}

CodecHeader make_header(const Image& image, const SerialOptions& options, bool compressed) {
    validate_image(image);
    return CodecHeader{
        {MAGIC_0, MAGIC_1},
        static_cast<uint8_t>(image.width / 256),
        static_cast<uint8_t>(image.height / 256),
        make_flags(options, compressed)
    };
}

uint32_t restore_width(const CodecHeader& h) { return static_cast<uint32_t>(h.width_256) * 256u; }
uint32_t restore_height(const CodecHeader& h) { return static_cast<uint32_t>(h.height_256) * 256u; }

std::vector<uint8_t> lzss_compress(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    if (in.empty()) return out;

    size_t pos = 0;
    while (pos < in.size()) {
        size_t ctrl_pos = out.size();
        out.push_back(0);
        uint8_t ctrl = 0;

        for (int bit = 0; bit < 8 && pos < in.size(); ++bit) {
            int best_len = 0, best_dist = 0;
            int start = static_cast<int>(pos) > LZ_WINDOW_SIZE ? static_cast<int>(pos) - LZ_WINDOW_SIZE : 0;

            for (int cand = static_cast<int>(pos) - 1; cand >= start; --cand) {
                int len = 0;
                while (len < LZ_MAX_MATCH &&
                       pos + static_cast<size_t>(len) < in.size() &&
                       in[static_cast<size_t>(cand + len)] == in[pos + static_cast<size_t>(len)])
                    ++len;
                if (len > best_len && len >= LZ_MIN_MATCH) {
                    best_len = len;
                    best_dist = static_cast<int>(pos) - cand;
                    if (len == LZ_MAX_MATCH) break;
                }
            }

            if (best_len >= LZ_MIN_MATCH) {
                uint16_t dist = static_cast<uint16_t>(best_dist - 1);
                uint8_t len = static_cast<uint8_t>(best_len - LZ_MIN_MATCH);
                out.push_back(static_cast<uint8_t>(dist & 0xFFu));
                out.push_back(static_cast<uint8_t>(((dist >> 8) & 0x0Fu) << 4 | (len & 0x0Fu)));
                pos += static_cast<size_t>(best_len);
            } else {
                ctrl |= static_cast<uint8_t>(1u << bit);
                out.push_back(in[pos++]);
            }
        }

        out[ctrl_pos] = ctrl;
    }

    return out;
}

std::vector<uint8_t> lzss_decompress(const std::vector<uint8_t>& in, size_t expected) {
    std::vector<uint8_t> out;
    out.reserve(expected);

    size_t pos = 0;
    while (pos < in.size() && out.size() < expected) {
        uint8_t ctrl = in[pos++];
        for (int bit = 0; bit < 8 && out.size() < expected; ++bit) {
            if (pos > in.size()) throw std::runtime_error("Corrupted LZSS stream");
            if (ctrl & (1u << bit)) {
                if (pos >= in.size()) throw std::runtime_error("Corrupted LZSS literal");
                out.push_back(in[pos++]);
            } else {
                if (pos + 1 >= in.size()) throw std::runtime_error("Corrupted LZSS match");
                uint8_t b1 = in[pos++], b2 = in[pos++];
                int dist = static_cast<int>(static_cast<uint16_t>(b1) | (static_cast<uint16_t>((b2 >> 4) & 0x0F) << 8)) + 1;
                int len = static_cast<int>(b2 & 0x0F) + LZ_MIN_MATCH;
                if (dist <= 0 || static_cast<size_t>(dist) > out.size())
                    throw std::runtime_error("Invalid LZSS back-reference");
                size_t start = out.size() - static_cast<size_t>(dist);
                for (int i = 0; i < len && out.size() < expected; ++i) out.push_back(out[start + static_cast<size_t>(i)]);
            }
        }
    }

    if (out.size() != expected) throw std::runtime_error("LZSS output size mismatch");
    return out;
}

struct HuffmanNode { uint64_t freq = 0; int symbol = -1, left = -1, right = -1; };
struct HuffmanCode { uint32_t bits = 0; uint8_t length = 0; };

class BitWriter {
public:
    void write_bit(uint8_t bit) {
        cur_ = static_cast<uint8_t>((cur_ << 1) | (bit & 1u));
        if (++count_ == 8) { bytes_.push_back(cur_); cur_ = 0; count_ = 0; }
    }
    void write_bits(uint32_t bits, uint8_t count) {
        for (int i = static_cast<int>(count) - 1; i >= 0; --i) write_bit(static_cast<uint8_t>((bits >> i) & 1u));
    }
    std::vector<uint8_t> finish() {
        if (count_) { cur_ <<= static_cast<uint8_t>(8 - count_); bytes_.push_back(cur_); }
        return bytes_;
    }
private:
    std::vector<uint8_t> bytes_;
    uint8_t cur_ = 0, count_ = 0;
};

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    bool read_bit(uint8_t& bit) {
        if (byte_ >= bytes_.size()) return false;
        bit = static_cast<uint8_t>((bytes_[byte_] >> (7 - bit_)) & 1u);
        if (++bit_ == 8) { bit_ = 0; ++byte_; }
        return true;
    }
private:
    const std::vector<uint8_t>& bytes_;
    size_t byte_ = 0;
    uint8_t bit_ = 0;
};

void build_lengths(const std::vector<HuffmanNode>& nodes, int node, int depth, std::array<uint8_t, 256>& lens) {
    const auto& n = nodes[static_cast<size_t>(node)];
    if (n.symbol >= 0) { lens[static_cast<size_t>(n.symbol)] = static_cast<uint8_t>(depth ? depth : 1); return; }
    build_lengths(nodes, n.left, depth + 1, lens);
    build_lengths(nodes, n.right, depth + 1, lens);
}

std::array<uint8_t, 256> make_huffman_lengths(const std::vector<uint8_t>& in) {
    std::array<uint64_t, 256> freq{};
    for (uint8_t b : in) ++freq[b];

    struct Q { uint64_t freq; int idx; bool operator>(const Q& o) const { return freq != o.freq ? freq > o.freq : idx > o.idx; } };
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> pq;
    std::vector<HuffmanNode> nodes;
    nodes.reserve(512);

    for (int s = 0; s < 256; ++s) if (freq[static_cast<size_t>(s)]) {
        nodes.push_back({freq[static_cast<size_t>(s)], s, -1, -1});
        pq.push({freq[static_cast<size_t>(s)], static_cast<int>(nodes.size() - 1)});
    }

    std::array<uint8_t, 256> lens{};
    if (pq.empty()) return lens;

    while (pq.size() > 1) {
        Q a = pq.top(); pq.pop();
        Q b = pq.top(); pq.pop();
        nodes.push_back({a.freq + b.freq, -1, a.idx, b.idx});
        pq.push({a.freq + b.freq, static_cast<int>(nodes.size() - 1)});
    }

    build_lengths(nodes, pq.top().idx, 0, lens);
    return lens;
}

std::array<HuffmanCode, 256> make_codes(const std::array<uint8_t, 256>& lens) {
    std::array<int, 33> bl{};
    for (uint8_t len : lens) {
        if (len > 32) throw std::runtime_error("Huffman code length exceeds supported maximum");
        if (len) ++bl[len];
    }

    std::array<uint32_t, 33> next{};
    uint32_t code = 0;
    for (int bits = 1; bits <= 32; ++bits) {
        code = (code + static_cast<uint32_t>(bl[bits - 1])) << 1;
        next[bits] = code;
    }

    std::array<HuffmanCode, 256> codes{};
    for (int s = 0; s < 256; ++s) {
        uint8_t len = lens[static_cast<size_t>(s)];
        if (len) codes[static_cast<size_t>(s)] = {next[len]++, len};
    }
    return codes;
}

std::vector<uint8_t> huffman_compress(const std::vector<uint8_t>& in) {
    auto lens = make_huffman_lengths(in);
    auto codes = make_codes(lens);

    std::vector<uint8_t> out;
    std::vector<std::pair<uint8_t, uint8_t>> used;
    used.reserve(256);
    for (int s = 0; s < 256; ++s) if (lens[static_cast<size_t>(s)]) used.push_back({static_cast<uint8_t>(s), lens[static_cast<size_t>(s)]});

    write_u16_le(out, static_cast<uint16_t>(used.size()));
    for (auto [sym, len] : used) { out.push_back(sym); out.push_back(len); }

    BitWriter bw;
    for (uint8_t b : in) {
        auto c = codes[b];
        bw.write_bits(c.bits, c.length);
    }

    auto bits = bw.finish();
    out.insert(out.end(), bits.begin(), bits.end());
    return out;
}

std::vector<uint8_t> huffman_decompress(const std::vector<uint8_t>& payload, size_t& pos, size_t expected) {
    std::array<uint8_t, 256> lens{};
    uint16_t used = read_u16_le(payload, pos);
    for (uint16_t i = 0; i < used; ++i) {
        if (pos + 2 > payload.size()) throw std::runtime_error("Corrupted Huffman header");
        uint8_t sym = payload[pos++], len = payload[pos++];
        if (!len || len > 32) throw std::runtime_error("Invalid Huffman code length");
        lens[sym] = len;
    }

    auto codes = make_codes(lens);

    struct Node { int child[2] = {-1, -1}; int sym = -1; };
    std::vector<Node> tree(1);

    for (int s = 0; s < 256; ++s) {
        auto c = codes[static_cast<size_t>(s)];
        if (!c.length) continue;
        int node = 0;
        for (int i = static_cast<int>(c.length) - 1; i >= 0; --i) {
            int bit = static_cast<int>((c.bits >> i) & 1u);
            if (tree[static_cast<size_t>(node)].child[bit] == -1) {
                tree[static_cast<size_t>(node)].child[bit] = static_cast<int>(tree.size());
                tree.push_back(Node{});
            }
            node = tree[static_cast<size_t>(node)].child[bit];
        }
        tree[static_cast<size_t>(node)].sym = s;
    }

    std::vector<uint8_t> bitstream(payload.begin() + static_cast<std::ptrdiff_t>(pos), payload.end());
    BitReader br(bitstream);
    std::vector<uint8_t> out;
    out.reserve(expected);

    while (out.size() < expected) {
        int node = 0;
        while (tree[static_cast<size_t>(node)].sym < 0) {
            uint8_t bit = 0;
            if (!br.read_bit(bit)) throw std::runtime_error("Unexpected end of Huffman bitstream");
            node = tree[static_cast<size_t>(node)].child[bit];
            if (node < 0) throw std::runtime_error("Invalid Huffman code in bitstream");
        }
        out.push_back(static_cast<uint8_t>(tree[static_cast<size_t>(node)].sym));
    }

    pos = payload.size();
    return out;
}

std::vector<uint8_t> compress_payload(const std::vector<uint8_t>& in) {
    auto lz = lzss_compress(in);
    std::vector<uint8_t> out;
    write_u32_le(out, static_cast<uint32_t>(lz.size()));
    auto huff = huffman_compress(lz);
    out.insert(out.end(), huff.begin(), huff.end());
    return out;
}

std::vector<uint8_t> decompress_payload(const std::vector<uint8_t>& in, size_t expected) {
    size_t pos = 0;
    uint32_t lz_size = read_u32_le(in, pos);
    auto lz = huffman_decompress(in, pos, static_cast<size_t>(lz_size));
    return lzss_decompress(lz, expected);
}

struct Candidate {
    SerialOptions options{};
    bool compressed = false;
    std::vector<uint8_t> payload;
    size_t total_size = 0;
};

Candidate build_candidate(const Image& image, const SerialOptions& options) {
    auto serialized = serialize_image(image, options);
    auto compressed = compress_payload(serialized);

    Candidate raw{options, false, serialized, HEADER_SIZE + serialized.size()};
    Candidate cmp{options, true, compressed, HEADER_SIZE + compressed.size()};
    return (cmp.total_size < raw.total_size) ? std::move(cmp) : std::move(raw);
}

const char* scan_name(ScanMode mode) {
    return mode == ScanMode::Horizontal ? "horizontal" : "vertical";
}

const char* model_name(ModelMode mode) {
    switch (mode) {
        case ModelMode::Raw: return "raw";
        case ModelMode::Delta: return "delta";
        case ModelMode::Paeth2D: return "paeth2d";
        default: return "unknown_model";
    }
}

std::vector<SerialOptions> candidate_options(bool adaptive_scan, bool use_model) {
    if (!adaptive_scan) {
        if (!use_model) return {{ScanMode::Horizontal, ModelMode::Raw}};
        return {
            {ScanMode::Horizontal, ModelMode::Raw},
            {ScanMode::Horizontal, ModelMode::Delta},
            {ScanMode::Horizontal, ModelMode::Paeth2D}
        };
    }

    if (!use_model) {
        return {
            {ScanMode::Horizontal, ModelMode::Raw},
            {ScanMode::Vertical,   ModelMode::Raw}
        };
    }

    return {
        {ScanMode::Horizontal, ModelMode::Raw},
        {ScanMode::Horizontal, ModelMode::Delta},
        {ScanMode::Horizontal, ModelMode::Paeth2D},
        {ScanMode::Vertical,   ModelMode::Raw},
        {ScanMode::Vertical,   ModelMode::Delta}
    };
}

} // namespace

uint8_t make_flags(const SerialOptions& options, bool compressed) {
    uint8_t flags = compressed ? FLAG_COMPRESSED : 0;
    if (options.scan_mode == ScanMode::Vertical) flags |= FLAG_VERTICAL;
    uint8_t model_bits = static_cast<uint8_t>(options.model_mode) << MODEL_SHIFT;
    if (model_bits & ~MODEL_MASK) throw std::runtime_error("Model mode does not fit into header flags");
    return static_cast<uint8_t>(flags | model_bits);
}

SerialOptions parse_flags(uint8_t flags) {
    validate_flags(flags);
    return {
        (flags & FLAG_VERTICAL) ? ScanMode::Vertical : ScanMode::Horizontal,
        static_cast<ModelMode>((flags & MODEL_MASK) >> MODEL_SHIFT)
    };
}

bool is_compressed(uint8_t flags) {
    validate_flags(flags);
    return (flags & FLAG_COMPRESSED) != 0;
}

CodecPackage encode_image(const Image& image, bool adaptive_scan, bool use_model) {
    validate_image(image);

    bool have_best = false;
    Candidate best{};

    for (const auto& options : candidate_options(adaptive_scan, use_model)) {
        Candidate c = build_candidate(image, options);
        if (!have_best || c.total_size < best.total_size) {
            best = std::move(c);
            have_best = true;
        }
    }

    if (!have_best) throw std::runtime_error("Failed to build encoding candidates");

    std::cout << "CHOICE scan=" << scan_name(best.options.scan_mode)
              << " model=" << model_name(best.options.model_mode)
              << " compressed=" << (best.compressed ? 1 : 0)
              << " size=" << best.total_size << "\n";

    return {make_header(image, best.options, best.compressed), std::move(best.payload)};
}

Image decode_image(const CodecPackage& package) {
    validate_magic(package.header);
    validate_flags(package.header.flags);

    uint32_t width = restore_width(package.header);
    uint32_t height = restore_height(package.header);
    size_t expected = pixel_count(width, height);

    std::vector<uint8_t> serialized = is_compressed(package.header.flags)
        ? decompress_payload(package.payload, expected)
        : package.payload;

    if (serialized.size() != expected)
        throw std::runtime_error("Decoded payload size does not match image dimensions");

    return deserialize_image(serialized, width, height, parse_flags(package.header.flags));
}

std::vector<uint8_t> pack_container(const CodecPackage& package) {
    validate_magic(package.header);
    validate_flags(package.header.flags);

    std::vector<uint8_t> out;
    out.reserve(HEADER_SIZE + package.payload.size());
    out.push_back(static_cast<uint8_t>(package.header.magic[0]));
    out.push_back(static_cast<uint8_t>(package.header.magic[1]));
    out.push_back(package.header.width_256);
    out.push_back(package.header.height_256);
    out.push_back(package.header.flags);
    out.insert(out.end(), package.payload.begin(), package.payload.end());
    return out;
}

CodecPackage unpack_container(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < HEADER_SIZE) throw std::runtime_error("Input is too small to be a valid container");

    CodecHeader header{};
    header.magic[0] = static_cast<char>(bytes[0]);
    header.magic[1] = static_cast<char>(bytes[1]);
    header.width_256 = bytes[2];
    header.height_256 = bytes[3];
    header.flags = bytes[4];

    validate_magic(header);
    validate_flags(header.flags);

    return {
        header,
        std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(HEADER_SIZE), bytes.end())
    };
}

void compress_file(const ParsedArgs& args) {
    Image image = read_raw_image(args.infile, args.width);
    CodecPackage package = encode_image(image, args.adaptive_scan, args.use_model);
    write_entire_file(args.outfile, pack_container(package));
}

void decompress_file(const ParsedArgs& args) {
    CodecPackage package = unpack_container(read_entire_file(args.infile));
    write_raw_image(args.outfile, decode_image(package));
}