#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "render3d/Camera3D.h"
#include "render3d/RenderVolume.h"

namespace vtc {

// One cut of the 3D scissors: a region drawn over the image removes the
// voxels behind it (or everything outside it), through the whole depth.
struct SculptCut {
    Camera3D camera;
    int width = 0;
    int height = 0;
    std::vector<std::array<double, 2>> polygon;  // image pixels
    bool removeInside = true;
};

// Pixels inside the polygon (even-odd rule), width x height, row by row.
std::vector<std::uint8_t> rasterizePolygon(const std::vector<std::array<double, 2>>& polygon, int width, int height);

// Copy of `source` with the cuts applied (removed voxels become empty).
// The original volume is never changed, so cuts can be undone.
std::shared_ptr<RenderVolume> applyCuts(const RenderVolume& source, const std::vector<SculptCut>& cuts);

}  // namespace vtc
