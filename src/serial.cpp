#include "../include/serial.hpp"
#include <cstdlib>
#include <stdexcept>

namespace {

size_t pixel_count(uint32_t w, uint32_t h) { return static_cast<size_t>(w) * h; }
size_t idx(uint32_t x, uint32_t y, uint32_t w) { return static_cast<size_t>(y) * w + x; }

void validate_image(const Image& image) {
    if (!image.width || !image.height) throw std::runtime_error("Image dimensions must be > 0");
    if (image.pixels.size() != pixel_count(image.width, image.height))
        throw std::runtime_error("Image pixel buffer size does not match image dimensions");
}

void validate_data(const std::vector<uint8_t>& data, uint32_t w, uint32_t h) {
    if (!w || !h) throw std::runtime_error("Image dimensions must be > 0");
    if (data.size() != pixel_count(w, h))
        throw std::runtime_error("Serialized data size does not match image dimensions");
}

std::vector<uint8_t> scan_raw(const Image& image, ScanMode mode) {
    if (mode == ScanMode::Horizontal) return image.pixels;
    std::vector<uint8_t> out;
    out.reserve(image.pixels.size());
    for (uint32_t x = 0; x < image.width; ++x)
        for (uint32_t y = 0; y < image.height; ++y)
            out.push_back(image.pixels[idx(x, y, image.width)]);
    return out;
}

Image unscan_raw(const std::vector<uint8_t>& data, uint32_t w, uint32_t h, ScanMode mode) {
    if (mode == ScanMode::Horizontal) return {w, h, data};
    std::vector<uint8_t> pixels(pixel_count(w, h));
    size_t k = 0;
    for (uint32_t x = 0; x < w; ++x)
        for (uint32_t y = 0; y < h; ++y)
            pixels[idx(x, y, w)] = data[k++];
    return {w, h, std::move(pixels)};
}

std::vector<uint8_t> delta_encode(const std::vector<uint8_t>& in) {
    if (in.empty()) return {};
    std::vector<uint8_t> out(in.size());
    out[0] = in[0];
    for (size_t i = 1; i < in.size(); ++i) out[i] = static_cast<uint8_t>(in[i] - in[i - 1]);
    return out;
}

std::vector<uint8_t> delta_decode(const std::vector<uint8_t>& in) {
    if (in.empty()) return {};
    std::vector<uint8_t> out(in.size());
    out[0] = in[0];
    for (size_t i = 1; i < in.size(); ++i) out[i] = static_cast<uint8_t>(out[i - 1] + in[i]);
    return out;
}

uint8_t left(const std::vector<uint8_t>& p, uint32_t x, uint32_t y, uint32_t w) {
    return x ? p[idx(x - 1, y, w)] : 0;
}

uint8_t top(const std::vector<uint8_t>& p, uint32_t x, uint32_t y, uint32_t w) {
    return y ? p[idx(x, y - 1, w)] : 0;
}

uint8_t paeth(const std::vector<uint8_t>& p, uint32_t x, uint32_t y, uint32_t w) {
    const int a = left(p, x, y, w);
    const int b = top(p, x, y, w);
    const int c = (x && y) ? p[idx(x - 1, y - 1, w)] : 0;
    const int pred = a + b - c;
    const int pa = std::abs(pred - a), pb = std::abs(pred - b), pc = std::abs(pred - c);
    return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
}

std::vector<uint8_t> paeth_encode(const Image& image) {
    std::vector<uint8_t> out(image.pixels.size());
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t i = idx(x, y, image.width);
            out[i] = static_cast<uint8_t>(image.pixels[i] - paeth(image.pixels, x, y, image.width));
        }
    return out;
}

Image paeth_decode(const std::vector<uint8_t>& residuals, uint32_t w, uint32_t h) {
    std::vector<uint8_t> pixels(pixel_count(w, h));
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = idx(x, y, w);
            pixels[i] = static_cast<uint8_t>(paeth(pixels, x, y, w) + residuals[i]);
        }
    return {w, h, std::move(pixels)};
}

} // namespace

std::vector<uint8_t> serialize_image(const Image& image, const SerialOptions& options) {
    validate_image(image);

    if (options.model_mode == ModelMode::Raw)
        return scan_raw(image, options.scan_mode);

    if (options.model_mode == ModelMode::Delta)
        return delta_encode(scan_raw(image, options.scan_mode));

    if (options.model_mode == ModelMode::Paeth2D)
        return scan_raw({image.width, image.height, paeth_encode(image)}, options.scan_mode);

    throw std::runtime_error("Unknown model mode");
}

Image deserialize_image(const std::vector<uint8_t>& data,
                        uint32_t width,
                        uint32_t height,
                        const SerialOptions& options) {
    validate_data(data, width, height);

    if (options.model_mode == ModelMode::Raw)
        return unscan_raw(data, width, height, options.scan_mode);

    if (options.model_mode == ModelMode::Delta)
        return unscan_raw(delta_decode(data), width, height, options.scan_mode);

    if (options.model_mode == ModelMode::Paeth2D) {
        Image residuals = unscan_raw(data, width, height, options.scan_mode);
        return paeth_decode(residuals.pixels, width, height);
    }

    throw std::runtime_error("Unknown model mode");
}