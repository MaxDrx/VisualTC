#pragma once

#include <array>

#include "render3d/TransferFunction.h"

namespace vtc {

// Part of the volume kept by the crop box: fractions of each volume axis.
struct CropBox {
    std::array<double, 3> lo{0.0, 0.0, 0.0};
    std::array<double, 3> hi{1.0, 1.0, 1.0};
    [[nodiscard]] bool isFull() const {
        return lo[0] <= 0.0 && lo[1] <= 0.0 && lo[2] <= 0.0 && hi[0] >= 1.0 && hi[1] >= 1.0 && hi[2] >= 1.0;
    }
    [[nodiscard]] bool operator==(const CropBox& o) const { return lo == o.lo && hi == o.hi; }
};

// Everything besides the volume, the transfer table and the camera that
// decides the image. Both renderers follow the same rules.
struct RenderSettings {
    RenderMode mode = RenderMode::Volume;
    bool shading = true;
    float ambient = 0.28F;
    float diffuse = 0.72F;
    float specular = 0.28F;
    float shininess = 24.0F;
    double windowCenter = 40.0;  // MIP, MinIP and mean: modality units
    double windowWidth = 400.0;
    double surfaceValue = 300.0;  // surface mode threshold, modality units
    std::array<float, 3> surfaceColor{0.92F, 0.86F, 0.74F};
    double stepVoxels = 0.5;  // sampling distance, in voxels of the smallest spacing
    CropBox crop;
    std::array<float, 3> background{0.0F, 0.0F, 0.0F};
    bool jitter = true;              // breaks the rings of regular sampling
    bool emptySpaceSkipping = true;  // off only to check that skipping changes nothing
    bool fastGradient = false;       // processor, while moving: cheaper lighting

    [[nodiscard]] double stepMm(double minSpacing) const { return stepVoxels * minSpacing; }
};

}  // namespace vtc
