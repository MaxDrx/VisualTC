#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "core/Vec3.h"
#include "render3d/Camera3D.h"
#include "render3d/RenderSettings.h"
#include "render3d/RenderVolume.h"
#include "render3d/TransferFunction.h"

namespace vtc {

// Ray casting on the processor: works on any computer, uses every core,
// skips empty blocks of the volume and stops rays that became opaque.
// The graphics card renderer reproduces the same rules in a shader.
class CpuRayCaster {
public:
    // Fills width x height pixels (0xAARRGGBB, opaque) of `out`. Returns
    // false when cancelled before the end (the image is then incomplete).
    static bool render(const RenderVolume& volume, const TransferLut& lut, const Camera3D& camera,
                       const RenderSettings& settings, int width, int height, std::uint32_t* out,
                       const std::atomic<bool>* cancel = nullptr, unsigned threads = 0);

    // Patient point of the structure seen at a pixel: where the volume
    // becomes half opaque, the surface hit or the maximum/minimum of the
    // projections. Nothing for the mean projection or an empty ray.
    static std::optional<Vec3> pick(const RenderVolume& volume, const TransferLut& lut, const Camera3D& camera,
                                    const RenderSettings& settings, int width, int height, double px, double py);
};

}  // namespace vtc
