#pragma once

#include <string>
#include <vector>

#include "core/Vec3.h"
#include "dicom/DicomTypes.h"

namespace vtc {

enum class GeometryIssue {
    MissingGeometry,       // some frame lacks position/orientation/spacing
    MixedOrientation,      // frames are not parallel
    MixedSize,             // rows/columns (or pixel spacing) differ
    MixedFrameOfReference, // frames use different coordinate systems
    DuplicatePositions,    // two frames at the same location
    MissingSlices,         // gaps larger than 1.5x the typical spacing
    IrregularSpacing,      // spacing varies (but no obvious gap)
    GantryTilt,            // slice origins shear in-plane (tilted gantry)
    TooFewSlices,          // fewer than 3 slices: no volume
    Color                  // multi-sample data: no volumetric reconstruction
};

std::string describe(GeometryIssue issue);  // pt-BR, user-facing

// Result of the spatial analysis of an ordered stack of frames.
struct StackGeometry {
    bool spatial = false;      // every frame has position, orientation and spacing
    bool parallel = false;     // same orientation and size for all frames
    bool volumetric = false;   // safe to build a volume (MPR / 3D)
    int rows = 0;
    int columns = 0;
    Vec3 rowDir{1, 0, 0};
    Vec3 colDir{0, 1, 0};
    Vec3 normal{0, 0, 1};
    double spacingX = 1.0;
    double spacingY = 1.0;
    // Signed position of every frame along `normal` (display order).
    std::vector<double> sliceOffsets;
    double sliceSpacing = 0.0;  // median absolute spacing (mm)
    double minSpacing = 0.0;
    double maxSpacing = 0.0;
    bool uniformSpacing = false;
    int gapCount = 0;
    double maxInPlaneShift = 0.0;  // mm, for gantry tilt detection
    double tiltDegrees = 0.0;
    std::string frameOfReferenceUid;
    std::vector<GeometryIssue> issues;

    [[nodiscard]] bool has(GeometryIssue i) const;
};

// Analyzes frames already in display order.
StackGeometry analyzeStack(const std::vector<FrameRef>& frames);

// True when both direction cosines agree within ~1 degree.
bool sameOrientation(const FrameGeometry& a, const FrameGeometry& b, double cosTolerance = 0.9998);

// True when both frames have the same calibration source and in-plane pixel
// spacing (relative tolerance 0.1%).
bool sameSpacing(const FrameGeometry& a, const FrameGeometry& b);

// Intersection of the plane (point, normal) with the segment [a, b].
bool intersectSegmentWithPlane(const Vec3& a, const Vec3& b, const Vec3& planePoint, const Vec3& planeNormal,
                               Vec3& out);

}  // namespace vtc
