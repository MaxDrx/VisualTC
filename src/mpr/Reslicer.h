#pragma once

#include <memory>

#include "dicom/DicomTypes.h"
#include "imaging/PixelData.h"
#include "mpr/ImageVolume.h"

namespace vtc {

// Output plane of a reslice. Pixel (i, j) is at origin + u*i*spacingU +
// v*j*spacingV, with u (columns, screen right) and v (rows, screen down)
// orthonormal.
struct ReslicePlane {
    Vec3 origin;
    Vec3 u{1, 0, 0};
    Vec3 v{0, 1, 0};
    double spacingU = 1.0;
    double spacingV = 1.0;
    int width = 0;
    int height = 0;

    [[nodiscard]] Vec3 normal() const { return u.cross(v).normalized(); }
    [[nodiscard]] Vec3 pixelToPatient(double i, double j) const {
        return origin + u * (i * spacingU) + v * (j * spacingV);
    }
    [[nodiscard]] FrameGeometry toFrameGeometry() const;
};

enum class SlabMode { Average, MIP, MinIP };

struct SlabParams {
    double thickness = 0.0;  // mm; 0 = single plane
    SlabMode mode = SlabMode::MIP;
};

// Samples the volume on a plane. Output: F32 frame of modality values
// (slope 1, intercept 0), NaN where the plane is outside the volume or inside
// a gap of missing slices. Multithreaded over output rows.
std::shared_ptr<DecodedFrame> reslice(const ImageVolume& volume, const ReslicePlane& plane, const SlabParams& slab,
                                      Interpolation interp);

}  // namespace vtc
