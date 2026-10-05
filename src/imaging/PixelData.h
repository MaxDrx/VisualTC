#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace vtc {

enum class PixelFormat { U8, I8, U16, I16, U32, I32, F32, RGB8 };

inline int bytesPerPixel(PixelFormat f) {
    switch (f) {
        case PixelFormat::U8:
        case PixelFormat::I8: return 1;
        case PixelFormat::U16:
        case PixelFormat::I16: return 2;
        case PixelFormat::U32:
        case PixelFormat::I32:
        case PixelFormat::F32: return 4;
        case PixelFormat::RGB8: return 3;
    }
    return 1;
}

// One decoded 2D frame. Monochrome frames keep the STORED values (already
// masked to BitsStored and sign-extended); modality values are obtained with
// value = raw * slope + intercept (e.g. Hounsfield Units for CT).
struct DecodedFrame {
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::U16;
    std::vector<std::uint8_t> data;
    double slope = 1.0;
    double intercept = 0.0;
    bool monochrome1 = false;
    int bitsStored = 16;
    double minRaw = 0.0;  // range of stored values (monochrome)
    double maxRaw = 0.0;

    [[nodiscard]] bool isColor() const { return format == PixelFormat::RGB8; }
    [[nodiscard]] std::size_t pixelCount() const {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    [[nodiscard]] std::size_t byteSize() const { return data.size() + sizeof(DecodedFrame); }

    template <class T>
    [[nodiscard]] const T* as() const {
        return reinterpret_cast<const T*>(data.data());
    }
    template <class T>
    [[nodiscard]] T* as() {
        return reinterpret_cast<T*>(data.data());
    }

    // Stored value at pixel index (monochrome). NaN for color.
    [[nodiscard]] double rawAt(std::size_t i) const {
        switch (format) {
            case PixelFormat::U8: return data[i];
            case PixelFormat::I8: return static_cast<std::int8_t>(data[i]);
            case PixelFormat::U16: return as<std::uint16_t>()[i];
            case PixelFormat::I16: return as<std::int16_t>()[i];
            case PixelFormat::U32: return as<std::uint32_t>()[i];
            case PixelFormat::I32: return as<std::int32_t>()[i];
            case PixelFormat::F32: return as<float>()[i];
            case PixelFormat::RGB8: break;
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    [[nodiscard]] double rawAt(int x, int y) const {
        return rawAt(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x));
    }
    // Modality value (HU for CT). NaN outside the image or for color.
    [[nodiscard]] double valueAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return rawAt(x, y) * slope + intercept;
    }
    void computeRange();
};

using DecodedFramePtr = std::shared_ptr<const DecodedFrame>;

}  // namespace vtc
