#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dicom/DicomTypes.h"
#include "imaging/PixelData.h"

namespace vtc {

enum class VoiFunction { Linear, LinearExact, Sigmoid };

VoiFunction parseVoiFunction(const std::string& dicomValue);

// VOI transformation of PS3.3 C.11.2.1.2, normalized to an output range of
// [0, 1]. `x` is a modality value (e.g. HU). Width must be >= 1 for LINEAR
// and > 0 for the other functions; invalid widths are clamped.
double applyVoi(double x, double center, double width, VoiFunction fn);

// Tabulated VOI LUT (VOI LUT Sequence): modality value -> [0, 1].
double applyVoiLut(double x, const VoiLutTable& lut);

struct WindowPreset {
    std::string name;
    double center = 0.0;
    double width = 1.0;
};

// Built-in CT presets (HU). Values from usual clinical practice; the DICOM
// window of the file, when present, is always the default (section 14).
const std::vector<WindowPreset>& ctPresets();

// Window derived from the data range (modality units) when the file has none.
WindowPreset autoWindow(const DecodedFrame& frame);

struct DisplayParams {
    double center = 127.5;
    double width = 256.0;
    VoiFunction function = VoiFunction::Linear;
    bool invert = false;                  // user inversion (XOR with MONOCHROME1)
    const VoiLutTable* voiLut = nullptr;  // overrides center/width when set
};

// Renders monochrome frames to 8-bit grey and color frames to 8-bit RGB with
// the VOI applied. For integer data up to 16 bits a lookup table indexed by
// the stored value is built once per parameter change, so window/level
// dragging stays interactive on large images.
class DisplayRenderer {
public:
    // out: width*height bytes (grey) with row stride `stride`.
    void renderGray(const DecodedFrame& frame, const DisplayParams& params, std::uint8_t* out, int stride);
    // out: width*height*4 bytes (0xFFRRGGBB per pixel, native endianness).
    void renderRgb32(const DecodedFrame& frame, const DisplayParams& params, std::uint32_t* out, int strideBytes);

private:
    struct LutKey {
        PixelFormat format = PixelFormat::U8;
        double slope = 0, intercept = 0, center = 0, width = 0;
        VoiFunction function = VoiFunction::Linear;
        bool invert = false;
        const VoiLutTable* voiLut = nullptr;
        bool operator==(const LutKey&) const = default;
    };
    void ensureLut(const DecodedFrame& frame, const DisplayParams& params, bool effectiveInvert);

    LutKey key_;
    bool valid_ = false;
    int lutOffset_ = 0;  // stored value -> index offset
    std::vector<std::uint8_t> lut_;
};

}  // namespace vtc
