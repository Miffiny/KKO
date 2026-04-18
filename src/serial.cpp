#include "../include/serial.hpp"

#include <stdexcept>

namespace {

size_t checked_pixel_count(const uint32_t width, const uint32_t height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height);
}

size_t pixel_index(const uint32_t x, const uint32_t y, const uint32_t width) {
    return static_cast<size_t>(y) * width + x;
}

void validate_image(const Image& image) {
    if (image.width == 0 || image.height == 0) {
        throw std::runtime_error("Image dimensions must be > 0");
    }

    const size_t expected_size = checked_pixel_count(image.width, image.height);
    if (image.pixels.size() != expected_size) {
        throw std::runtime_error("Image pixel buffer size does not match image dimensions");
    }
}

void validate_serialized_data(const std::vector<uint8_t>& data,
                              const uint32_t width,
                              const uint32_t height) {
    if (width == 0 || height == 0) {
        throw std::runtime_error("Image dimensions must be > 0");
    }

    const size_t expected_size = checked_pixel_count(width, height);
    if (data.size() != expected_size) {
        throw std::runtime_error("Serialized data size does not match image dimensions");
    }
}

// -----------------------------------------------------------------------------
// 1D serialization helpers
// -----------------------------------------------------------------------------

std::vector<uint8_t> serialize_horizontal_raw(const Image& image) {
    return image.pixels;
}

std::vector<uint8_t> serialize_vertical_raw(const Image& image) {
    std::vector<uint8_t> out;
    out.reserve(image.pixels.size());

    for (uint32_t x = 0; x < image.width; ++x) {
        for (uint32_t y = 0; y < image.height; ++y) {
            out.push_back(image.pixels[pixel_index(x, y, image.width)]);
        }
    }

    return out;
}

Image deserialize_horizontal_raw(const std::vector<uint8_t>& data,
                                 const uint32_t width,
                                 const uint32_t height) {
    return Image{width, height, data};
}

Image deserialize_vertical_raw(const std::vector<uint8_t>& data,
                               const uint32_t width,
                               const uint32_t height) {
    std::vector<uint8_t> pixels(checked_pixel_count(width, height));

    size_t idx = 0;
    for (uint32_t x = 0; x < width; ++x) {
        for (uint32_t y = 0; y < height; ++y) {
            pixels[pixel_index(x, y, width)] = data[idx++];
        }
    }

    return Image{width, height, std::move(pixels)};
}

std::vector<uint8_t> serialize_raw_by_scan(const Image& image, const ScanMode mode) {
    switch (mode) {
        case ScanMode::Horizontal:
            return serialize_horizontal_raw(image);
        case ScanMode::Vertical:
            return serialize_vertical_raw(image);
        default:
            throw std::runtime_error("Unknown scan mode");
    }
}

Image deserialize_raw_by_scan(const std::vector<uint8_t>& data,
                              const uint32_t width,
                              const uint32_t height,
                              const ScanMode mode) {
    switch (mode) {
        case ScanMode::Horizontal:
            return deserialize_horizontal_raw(data, width, height);
        case ScanMode::Vertical:
            return deserialize_vertical_raw(data, width, height);
        default:
            throw std::runtime_error("Unknown scan mode");
    }
}

// -----------------------------------------------------------------------------
// Current 1D delta model
// -----------------------------------------------------------------------------

std::vector<uint8_t> apply_delta_model(const std::vector<uint8_t>& input) {
    if (input.empty()) {
        return {};
    }

    std::vector<uint8_t> out(input.size());
    out[0] = input[0];

    for (size_t i = 1; i < input.size(); ++i) {
        out[i] = static_cast<uint8_t>(input[i] - input[i - 1]);
    }

    return out;
}

std::vector<uint8_t> invert_delta_model(const std::vector<uint8_t>& input) {
    if (input.empty()) {
        return {};
    }

    std::vector<uint8_t> out(input.size());
    out[0] = input[0];

    for (size_t i = 1; i < input.size(); ++i) {
        out[i] = static_cast<uint8_t>(out[i - 1] + input[i]);
    }

    return out;
}

// -----------------------------------------------------------------------------
// 2D predictor helpers
// -----------------------------------------------------------------------------

uint8_t predict_left(const std::vector<uint8_t>& pixels,
                     const uint32_t x,
                     const uint32_t y,
                     const uint32_t width) {
    if (x == 0) {
        return 0;
    }
    return pixels[pixel_index(x - 1, y, width)];
}

uint8_t predict_top(const std::vector<uint8_t>& pixels,
                    const uint32_t x,
                    const uint32_t y,
                    const uint32_t width) {
    if (y == 0) {
        return 0;
    }
    return pixels[pixel_index(x, y - 1, width)];
}

uint8_t predict_average(const std::vector<uint8_t>& pixels,
                        const uint32_t x,
                        const uint32_t y,
                        const uint32_t width) {
    const uint8_t left = predict_left(pixels, x, y, width);
    const uint8_t top  = predict_top(pixels, x, y, width);
    return static_cast<uint8_t>((static_cast<unsigned>(left) +
                                 static_cast<unsigned>(top)) / 2u);
}

std::vector<uint8_t> encode_left_2d(const Image& image) {
    std::vector<uint8_t> residuals(image.pixels.size());

    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t idx = pixel_index(x, y, image.width);
            const uint8_t pred = predict_left(image.pixels, x, y, image.width);
            residuals[idx] = static_cast<uint8_t>(image.pixels[idx] - pred);
        }
    }

    return residuals;
}

std::vector<uint8_t> encode_top_2d(const Image& image) {
    std::vector<uint8_t> residuals(image.pixels.size());

    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t idx = pixel_index(x, y, image.width);
            const uint8_t pred = predict_top(image.pixels, x, y, image.width);
            residuals[idx] = static_cast<uint8_t>(image.pixels[idx] - pred);
        }
    }

    return residuals;
}

std::vector<uint8_t> encode_average_2d(const Image& image) {
    std::vector<uint8_t> residuals(image.pixels.size());

    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t idx = pixel_index(x, y, image.width);
            const uint8_t pred = predict_average(image.pixels, x, y, image.width);
            residuals[idx] = static_cast<uint8_t>(image.pixels[idx] - pred);
        }
    }

    return residuals;
}

Image decode_left_2d(const std::vector<uint8_t>& residuals,
                     const uint32_t width,
                     const uint32_t height) {
    std::vector<uint8_t> pixels(checked_pixel_count(width, height));

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = pixel_index(x, y, width);
            const uint8_t pred = predict_left(pixels, x, y, width);
            pixels[idx] = static_cast<uint8_t>(pred + residuals[idx]);
        }
    }

    return Image{width, height, std::move(pixels)};
}

Image decode_top_2d(const std::vector<uint8_t>& residuals,
                    const uint32_t width,
                    const uint32_t height) {
    std::vector<uint8_t> pixels(checked_pixel_count(width, height));

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = pixel_index(x, y, width);
            const uint8_t pred = predict_top(pixels, x, y, width);
            pixels[idx] = static_cast<uint8_t>(pred + residuals[idx]);
        }
    }

    return Image{width, height, std::move(pixels)};
}

Image decode_average_2d(const std::vector<uint8_t>& residuals,
                        const uint32_t width,
                        const uint32_t height) {
    std::vector<uint8_t> pixels(checked_pixel_count(width, height));

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t idx = pixel_index(x, y, width);
            const uint8_t pred = predict_average(pixels, x, y, width);
            pixels[idx] = static_cast<uint8_t>(pred + residuals[idx]);
        }
    }

    return Image{width, height, std::move(pixels)};
}

std::vector<uint8_t> encode_2d_model(const Image& image, const ModelMode mode) {
    switch (mode) {
        case ModelMode::Left2D:
            return encode_left_2d(image);
        case ModelMode::Top2D:
            return encode_top_2d(image);
        case ModelMode::Average2D:
            return encode_average_2d(image);
        default:
            throw std::runtime_error("Unknown 2D model mode");
    }
}

Image decode_2d_model(const std::vector<uint8_t>& residuals,
                      const uint32_t width,
                      const uint32_t height,
                      const ModelMode mode) {
    switch (mode) {
        case ModelMode::Left2D:
            return decode_left_2d(residuals, width, height);
        case ModelMode::Top2D:
            return decode_top_2d(residuals, width, height);
        case ModelMode::Average2D:
            return decode_average_2d(residuals, width, height);
        default:
            throw std::runtime_error("Unknown 2D model mode");
    }
}

bool is_2d_model(const ModelMode mode) {
    return mode == ModelMode::Left2D ||
           mode == ModelMode::Top2D ||
           mode == ModelMode::Average2D;
}

} // namespace

std::vector<uint8_t> serialize_image(const Image& image, const SerialOptions& options) {
    validate_image(image);

    if (is_2d_model(options.model_mode)) {
        Image residual_image{
            image.width,
            image.height,
            encode_2d_model(image, options.model_mode)
        };
        return serialize_raw_by_scan(residual_image, options.scan_mode);
    }

    std::vector<uint8_t> serialized = serialize_raw_by_scan(image, options.scan_mode);

    switch (options.model_mode) {
        case ModelMode::Raw:
            return serialized;

        case ModelMode::Delta:
            return apply_delta_model(serialized);

        default:
            throw std::runtime_error("Unknown model mode");
    }
}

Image deserialize_image(const std::vector<uint8_t>& data,
                        const uint32_t width,
                        const uint32_t height,
                        const SerialOptions& options) {
    validate_serialized_data(data, width, height);

    if (is_2d_model(options.model_mode)) {
        const Image residual_image =
            deserialize_raw_by_scan(data, width, height, options.scan_mode);

        return decode_2d_model(residual_image.pixels, width, height, options.model_mode);
    }

    std::vector<uint8_t> restored_stream;

    switch (options.model_mode) {
        case ModelMode::Raw:
            restored_stream = data;
            break;

        case ModelMode::Delta:
            restored_stream = invert_delta_model(data);
            break;

        default:
            throw std::runtime_error("Unknown model mode");
    }

    return deserialize_raw_by_scan(restored_stream, width, height, options.scan_mode);
}