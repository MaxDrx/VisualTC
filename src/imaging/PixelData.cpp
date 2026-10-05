#include "imaging/PixelData.h"

#include <algorithm>

namespace vtc {

namespace {
template <class T>
void rangeOf(const T* p, std::size_t n, double& lo, double& hi) {
    if (n == 0) {
        lo = hi = 0.0;
        return;
    }
    T mn = p[0];
    T mx = p[0];
    for (std::size_t i = 1; i < n; ++i) {
        mn = std::min(mn, p[i]);
        mx = std::max(mx, p[i]);
    }
    lo = static_cast<double>(mn);
    hi = static_cast<double>(mx);
}

void rangeOfFloat(const float* p, std::size_t n, double& lo, double& hi) {
    bool any = false;
    float mn = 0.0F;
    float mx = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        const float v = p[i];
        if (std::isnan(v)) {
            continue;
        }
        if (!any) {
            mn = mx = v;
            any = true;
        } else {
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
    }
    lo = mn;
    hi = mx;
}
}  // namespace

void DecodedFrame::computeRange() {
    const std::size_t n = pixelCount();
    switch (format) {
        case PixelFormat::U8: rangeOf(as<std::uint8_t>(), n, minRaw, maxRaw); break;
        case PixelFormat::I8: rangeOf(as<std::int8_t>(), n, minRaw, maxRaw); break;
        case PixelFormat::U16: rangeOf(as<std::uint16_t>(), n, minRaw, maxRaw); break;
        case PixelFormat::I16: rangeOf(as<std::int16_t>(), n, minRaw, maxRaw); break;
        case PixelFormat::U32: rangeOf(as<std::uint32_t>(), n, minRaw, maxRaw); break;
        case PixelFormat::I32: rangeOf(as<std::int32_t>(), n, minRaw, maxRaw); break;
        case PixelFormat::F32: rangeOfFloat(as<float>(), n, minRaw, maxRaw); break;
        case PixelFormat::RGB8:
            minRaw = 0.0;
            maxRaw = 255.0;
            break;
    }
}

}  // namespace vtc
