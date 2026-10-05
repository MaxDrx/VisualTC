#include "imaging/WindowLevel.h"

#include <algorithm>
#include <cmath>

namespace vtc {

VoiFunction parseVoiFunction(const std::string& v) {
    if (v == "LINEAR_EXACT") {
        return VoiFunction::LinearExact;
    }
    if (v == "SIGMOID") {
        return VoiFunction::Sigmoid;
    }
    return VoiFunction::Linear;
}

double applyVoi(double x, double c, double w, VoiFunction fn) {
    switch (fn) {
        case VoiFunction::Linear: {
            w = std::max(w, 1.0);
            const double lower = c - 0.5 - (w - 1.0) / 2.0;
            const double upper = c - 0.5 + (w - 1.0) / 2.0;
            if (x <= lower) {
                return 0.0;
            }
            if (x > upper) {
                return 1.0;
            }
            return (x - (c - 0.5)) / (w - 1.0) + 0.5;
        }
        case VoiFunction::LinearExact: {
            w = std::max(w, 1e-6);
            if (x <= c - w / 2.0) {
                return 0.0;
            }
            if (x > c + w / 2.0) {
                return 1.0;
            }
            return (x - c) / w + 0.5;
        }
        case VoiFunction::Sigmoid: {
            w = std::max(w, 1e-6);
            return 1.0 / (1.0 + std::exp(-4.0 * (x - c) / w));
        }
    }
    return 0.0;
}

double applyVoiLut(double x, const VoiLutTable& lut) {
    if (lut.data.empty()) {
        return 0.0;
    }
    const double maxOut = std::pow(2.0, lut.bitsPerEntry) - 1.0;
    const long idx = std::lround(x) - lut.firstMapped;
    const long clamped = std::clamp<long>(idx, 0, static_cast<long>(lut.data.size()) - 1);
    return std::clamp(lut.data[static_cast<size_t>(clamped)] / maxOut, 0.0, 1.0);
}

const std::vector<WindowPreset>& ctPresets() {
    static const std::vector<WindowPreset> presets = {
        {"Pulmão", -600.0, 1500.0},  {"Mediastino", 40.0, 400.0}, {"Abdome", 50.0, 400.0},
        {"Fígado", 30.0, 150.0},     {"Osso", 400.0, 1800.0},     {"Cérebro", 40.0, 80.0},
        {"Subdural", 75.0, 200.0},   {"AVC", 40.0, 40.0},
    };
    return presets;
}

WindowPreset autoWindow(const DecodedFrame& frame) {
    WindowPreset p;
    p.name = "Auto";
    if (frame.isColor()) {
        p.center = 127.5;
        p.width = 256.0;
        return p;
    }
    const std::size_t n = frame.pixelCount();
    if (n == 0) {
        return p;
    }
    // Robust range: 0.5% .. 99.5% percentiles over a subsample.
    const std::size_t step = std::max<std::size_t>(1, n / 65536);
    std::vector<double> samples;
    samples.reserve(n / step + 1);
    for (std::size_t i = 0; i < n; i += step) {
        const double v = frame.rawAt(i);
        if (!std::isnan(v)) {
            samples.push_back(v * frame.slope + frame.intercept);
        }
    }
    if (samples.empty()) {
        return p;
    }
    std::sort(samples.begin(), samples.end());
    const double lo = samples[static_cast<std::size_t>(0.005 * static_cast<double>(samples.size() - 1))];
    const double hi = samples[static_cast<std::size_t>(0.995 * static_cast<double>(samples.size() - 1))];
    p.width = std::max(1.0, hi - lo);
    p.center = lo + (hi - lo) / 2.0;
    return p;
}

void DisplayRenderer::ensureLut(const DecodedFrame& frame, const DisplayParams& params, bool effectiveInvert) {
    LutKey key;
    key.format = frame.format;
    key.slope = frame.slope;
    key.intercept = frame.intercept;
    key.center = params.center;
    key.width = params.width;
    key.function = params.function;
    key.invert = effectiveInvert;
    key.voiLut = params.voiLut;
    if (valid_ && key == key_) {
        return;
    }
    int lo = 0;
    int hi = 255;
    switch (frame.format) {
        case PixelFormat::U8:
        case PixelFormat::RGB8: lo = 0; hi = 255; break;
        case PixelFormat::I8: lo = -128; hi = 127; break;
        case PixelFormat::U16: lo = 0; hi = 65535; break;
        case PixelFormat::I16: lo = -32768; hi = 32767; break;
        default: valid_ = false; return;
    }
    lut_.resize(static_cast<std::size_t>(hi - lo + 1));
    for (int v = lo; v <= hi; ++v) {
        double y = 0.0;
        if (frame.format == PixelFormat::RGB8) {
            y = applyVoi(v, params.center, params.width, params.function);
        } else {
            const double m = v * frame.slope + frame.intercept;
            y = params.voiLut != nullptr ? applyVoiLut(m, *params.voiLut)
                                         : applyVoi(m, params.center, params.width, params.function);
        }
        if (effectiveInvert) {
            y = 1.0 - y;
        }
        lut_[static_cast<std::size_t>(v - lo)] = static_cast<std::uint8_t>(std::lround(y * 255.0));
    }
    lutOffset_ = -lo;
    key_ = key;
    valid_ = true;
}

void DisplayRenderer::renderGray(const DecodedFrame& frame, const DisplayParams& params, std::uint8_t* out,
                                 int stride) {
    const bool invert = params.invert != frame.monochrome1;
    const int w = frame.width;
    const int h = frame.height;
    ensureLut(frame, params, invert);
    auto rowLoop = [&](auto* src) {
        for (int y = 0; y < h; ++y) {
            std::uint8_t* dst = out + static_cast<std::ptrdiff_t>(y) * stride;
            const auto* s = src + static_cast<std::ptrdiff_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                dst[x] = lut_[static_cast<std::size_t>(static_cast<int>(s[x]) + lutOffset_)];
            }
        }
    };
    switch (frame.format) {
        case PixelFormat::U8: rowLoop(frame.as<std::uint8_t>()); return;
        case PixelFormat::I8: rowLoop(frame.as<std::int8_t>()); return;
        case PixelFormat::U16: rowLoop(frame.as<std::uint16_t>()); return;
        case PixelFormat::I16: rowLoop(frame.as<std::int16_t>()); return;
        default: break;
    }
    // Generic path (32-bit and float data): evaluate per pixel. NaN = black.
    for (int y = 0; y < h; ++y) {
        std::uint8_t* dst = out + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            const double raw = frame.rawAt(x, y);
            if (std::isnan(raw)) {
                dst[x] = 0;
                continue;
            }
            const double m = raw * frame.slope + frame.intercept;
            double v = params.voiLut != nullptr ? applyVoiLut(m, *params.voiLut)
                                                : applyVoi(m, params.center, params.width, params.function);
            if (invert) {
                v = 1.0 - v;
            }
            dst[x] = static_cast<std::uint8_t>(std::lround(v * 255.0));
        }
    }
}

void DisplayRenderer::renderRgb32(const DecodedFrame& frame, const DisplayParams& params, std::uint32_t* out,
                                  int strideBytes) {
    const int w = frame.width;
    const int h = frame.height;
    auto* base = reinterpret_cast<std::uint8_t*>(out);
    if (!frame.isColor()) {
        std::vector<std::uint8_t> grey(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        renderGray(frame, params, grey.data(), w);
        for (int y = 0; y < h; ++y) {
            auto* dst = reinterpret_cast<std::uint32_t*>(base + static_cast<std::ptrdiff_t>(y) * strideBytes);
            for (int x = 0; x < w; ++x) {
                const std::uint32_t g = grey[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                             static_cast<std::size_t>(x)];
                dst[x] = 0xFF000000u | (g << 16) | (g << 8) | g;
            }
        }
        return;
    }
    const bool identity = params.center == 127.5 && params.width == 256.0 && !params.invert &&
                          params.function == VoiFunction::Linear;
    if (!identity) {
        ensureLut(frame, params, params.invert);
    }
    const std::uint8_t* src = frame.data.data();
    for (int y = 0; y < h; ++y) {
        auto* dst = reinterpret_cast<std::uint32_t*>(base + static_cast<std::ptrdiff_t>(y) * strideBytes);
        const std::uint8_t* s = src + static_cast<std::ptrdiff_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) {
            std::uint32_t r = s[3 * x];
            std::uint32_t g = s[3 * x + 1];
            std::uint32_t b = s[3 * x + 2];
            if (!identity) {
                r = lut_[r];
                g = lut_[g];
                b = lut_[b];
            }
            dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

}  // namespace vtc
