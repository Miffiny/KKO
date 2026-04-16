#include "../include/serial.hpp"

#include <stdexcept>

namespace {

size_t checked_pixel_count(const uint32_t width, const uint32_t height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height);
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

std::vector<uint8_t> serialize_horizontal_raw(const Image& image) {
    return image.pixels;
}

std::vector<uint8_t> serialize_vertical_raw(const Image& image) {
    std::vector<uint8_t> out;
    out.reserve(image.pixels.size());

    for (uint32_t x = 0; x < image.width; ++x) {
        for (uint32_t y = 0; y < image.height; ++y) {
            out.push_back(image.pixels[static_cast<size_t>(y) * image.width + x]);
        }
    }

    return out;
}

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

Image deserialize_horizontal_raw(const std::vector<uint8_t>& data,
                                 uint32_t width,
                                 uint32_t height) {
    return Image{width, height, data};
}

Image deserialize_vertical_raw(const std::vector<uint8_t>& data,
                               uint32_t width,
                               uint32_t height) {
    std::vector<uint8_t> pixels(checked_pixel_count(width, height));

    size_t idx = 0;
    for (uint32_t x = 0; x < width; ++x) {
        for (uint32_t y = 0; y < height; ++y) {
            pixels[static_cast<size_t>(y) * width + x] = data[idx++];
        }
    }

    return Image{width, height, std::move(pixels)};
}

} // namespace

std::vector<uint8_t> serialize_image(const Image& image, const SerialOptions& options) {
    validate_image(image);

    std::vector<uint8_t> serialized;

    switch (options.scan_mode) {
        case ScanMode::Horizontal:
            serialized = serialize_horizontal_raw(image);
            break;

        case ScanMode::Vertical:
            serialized = serialize_vertical_raw(image);
            break;

        default:
            throw std::runtime_error("Unknown scan mode");
    }

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
                        uint32_t width,
                        uint32_t height,
                        const SerialOptions& options) {
    validate_serialized_data(data, width, height);

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

    switch (options.scan_mode) {
        case ScanMode::Horizontal:
            return deserialize_horizontal_raw(restored_stream, width, height);

        case ScanMode::Vertical:
            return deserialize_vertical_raw(restored_stream, width, height);

        default:
            throw std::runtime_error("Unknown scan mode");
    }
}