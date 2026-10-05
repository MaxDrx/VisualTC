#pragma once

#include <string>

#include "mpr/ImageVolume.h"
#include "mpr/Reslicer.h"

namespace vtc {

enum class MprOrientation { Axial, Coronal, Sagittal };

std::string toLabel(MprOrientation o);  // pt-BR

// One MPR viewing direction over a volume: an orthonormal frame (u right,
// v down, n = u x v) plus the extents of the volume in that frame.
struct MprView {
    MprOrientation orientation = MprOrientation::Axial;
    Vec3 u{1, 0, 0};
    Vec3 v{0, 1, 0};
    Vec3 n{0, 0, 1};
    double spacingU = 1.0;
    double spacingV = 1.0;
    double step = 1.0;  // scroll increment along n (mm)
    double uMin = 0, uMax = 0, vMin = 0, vMax = 0, nMin = 0, nMax = 0;
    int width = 0;
    int height = 0;
    int sliceCount = 0;

    [[nodiscard]] ReslicePlane planeThrough(const Vec3& point) const;
    [[nodiscard]] int sliceIndexOf(const Vec3& point) const;
    // Point moved so that its projection on n lands on slice `index`.
    [[nodiscard]] Vec3 pointAtSlice(const Vec3& point, int index) const;
};

// Orthogonal view aligned with the volume's own axes, choosing for each
// anatomical plane the volume axis closest to it. For a standard axial CT
// this gives exact native axial slices and true coronal/sagittal planes; for
// oblique acquisitions the planes stay aligned with the acquisition (so the
// native plane is never resampled) and orientation labels show the real
// obliquity.
MprView makeOrthogonalView(const ImageVolume& volume, MprOrientation orientation);

// Same as above but with an arbitrary orthonormal (u, v) frame (oblique MPR).
MprView makeView(const ImageVolume& volume, MprOrientation orientation, const Vec3& u, const Vec3& v);

}  // namespace vtc
