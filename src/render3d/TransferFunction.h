#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vtc {

class RenderVolume;

// How the rays combine the samples along their path.
enum class RenderMode {
    Volume,   // volume rendering: colour and opacity accumulated (VR)
    Surface,  // first crossing of a threshold, shaded (isosurface)
    Mip,      // maximum intensity projection
    MinIp,    // minimum intensity projection
    Average,  // mean along the ray (simulated radiograph)
};

// Opacities are given for a path of this length, so the image does not
// change with the sampling distance or with the resolution of the volume.
constexpr double kOpacityReferenceMm = 1.0;

struct TfPoint {
    double value = 0.0;  // modality units (HU for CT)
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float opacity = 0.0F;  // 0..1 for kOpacityReferenceMm of path
};

// Piecewise-linear colour and opacity over the modality values.
class TransferFunction {
public:
    std::vector<TfPoint> points;  // ascending values

    void sort();
    [[nodiscard]] std::array<float, 4> evaluate(double value) const;
    // Every point moved as a window: v' = pivot + (v - pivot) * scale + shift.
    [[nodiscard]] TransferFunction transformed(double shift, double scale, double pivot) const;
    // Middle of the values with some opacity (pivot of the window drag).
    [[nodiscard]] double center() const;
    [[nodiscard]] bool operator==(const TransferFunction& o) const;
};

// Table of 4096 entries over the quantized range of a RenderVolume, with the
// opacity already corrected for the sampling distance. Shared by both
// renderers: the CPU reads it directly, the GPU uploads it as a texture.
struct TransferLut {
    static constexpr int kSize = 4096;
    std::vector<float> rgba;           // kSize * 4
    std::vector<std::int32_t> opaque;  // prefix count of entries with opacity
    double stepMm = 1.0;

    // True when some quantized value in [qMin, qMax] is not transparent.
    [[nodiscard]] bool anyOpaque(int qMin, int qMax) const;
};

TransferLut buildTransferLut(const TransferFunction& tf, const RenderVolume& volume, double stepMm,
                             float opacityScale = 1.0F);

// Ready-made settings, as in the usual 3D workstations.
struct RenderPreset {
    enum class Applies { Ct, Mr, Any };
    std::string key;  // stable id, saved in the settings
    Applies applies = Applies::Any;
    RenderMode mode = RenderMode::Volume;
    // MR intensities have no absolute scale: values given as fractions of the
    // range between the 1st and 99.5th percentiles of the series.
    bool relative = false;
    std::vector<TfPoint> points;
    bool shading = true;
    double windowCenter = 0.0;  // projection modes (relative when `relative`)
    double windowWidth = 0.0;
    double surfaceValue = 0.0;  // surface mode threshold
    std::array<float, 3> surfaceColor{0.92F, 0.86F, 0.74F};
};

const std::vector<RenderPreset>& renderPresets();
const RenderPreset* findRenderPreset(const std::string& key);

// Preset values in the units of `volume` (relative MR values resolved).
struct ResolvedPreset {
    TransferFunction tf;
    double windowCenter = 0.0;
    double windowWidth = 1.0;
    double surfaceValue = 0.0;
};
ResolvedPreset resolvePreset(const RenderPreset& preset, const RenderVolume& volume);

}  // namespace vtc
