#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "dicom/DicomTypes.h"
#include "measurements/MeasurementMath.h"

namespace vtc {

// A finite image plane: frame geometry plus its pixel extent.
struct PlaneRect {
    FrameGeometry geometry;
    int width = 0;
    int height = 0;
    std::string frameOfReferenceUid;

    [[nodiscard]] Vec3 center() const {
        return geometry.pixelToPatient((width - 1) / 2.0, (height - 1) / 2.0);
    }
    [[nodiscard]] std::vector<Vec3> corners() const;
};

// True when both planes can be related spatially: same non-empty Frame of
// Reference and complete geometry. Different FoRs are NEVER synchronized
// (their coordinates are unrelated), even if the numbers look similar.
bool spatiallyComparable(const PlaneRect& a, const PlaneRect& b);

bool planesParallel(const Vec3& n1, const Vec3& n2, double maxAngleDegrees = 20.0);

// Index of the frame (in `frames`, display order) whose plane is closest to
// `point` along that frame's normal. Returns nullopt when the point lies
// further than `tolerance` mm outside the stack.
std::optional<int> nearestSliceIndex(const std::vector<FrameGeometry>& frames, const Vec3& point,
                                     double tolerance);

// Cross-reference line: intersection of plane `other` (finite rectangle)
// with plane `target`, expressed in `target`'s pixel coordinates. Returns
// nullopt for parallel planes or when they do not intersect.
std::optional<std::pair<Point2, Point2>> referenceLine(const PlaneRect& other, const PlaneRect& target);

// Continuous pixel coordinates of a patient point projected on a plane.
Point2 patientToPixel(const FrameGeometry& g, const Vec3& p);

}  // namespace vtc
