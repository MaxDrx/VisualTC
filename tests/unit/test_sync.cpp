#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "synchronization/SpatialSync.h"

using namespace vtc;

namespace {
FrameGeometry plane(Vec3 pos, Vec3 row, Vec3 col, double s = 1.0) {
    FrameGeometry g;
    g.hasPosition = g.hasOrientation = true;
    g.position = pos;
    g.rowDir = row;
    g.colDir = col;
    g.spacingX = g.spacingY = s;
    g.spacingSource = SpacingSource::PixelSpacing;
    return g;
}
}  // namespace

TEST_CASE("Spatial sync picks the anatomically nearest slice, not the same index", "[sync]") {
    // Target series: 5 mm slices from z = 0 to 100 (21 slices), feet-to-head order.
    std::vector<FrameGeometry> target;
    for (int i = 0; i <= 20; ++i) {
        target.push_back(plane({0, 0, i * 5.0}, {1, 0, 0}, {0, 1, 0}));
    }
    REQUIRE(nearestSliceIndex(target, {10, 20, 42.0}, 5.0) == 8);   // z=40
    REQUIRE(nearestSliceIndex(target, {0, 0, 43.0}, 5.0) == 9);     // z=45
    REQUIRE_FALSE(nearestSliceIndex(target, {0, 0, 200.0}, 5.0).has_value());
    // Reverse display order must give the reversed index.
    std::vector<FrameGeometry> reversed(target.rbegin(), target.rend());
    REQUIRE(nearestSliceIndex(reversed, {0, 0, 42.0}, 5.0) == 12);
}

TEST_CASE("Different Frames of Reference are never comparable", "[sync]") {
    PlaneRect a{plane({0, 0, 0}, {1, 0, 0}, {0, 1, 0}), 10, 10, "1.2.3"};
    PlaneRect b{plane({0, 0, 0}, {1, 0, 0}, {0, 1, 0}), 10, 10, "1.2.4"};
    PlaneRect c{plane({0, 0, 0}, {1, 0, 0}, {0, 1, 0}), 10, 10, ""};
    REQUIRE_FALSE(spatiallyComparable(a, b));
    REQUIRE_FALSE(spatiallyComparable(a, c));
    b.frameOfReferenceUid = "1.2.3";
    REQUIRE(spatiallyComparable(a, b));
}

TEST_CASE("Reference line of an axial slice on a coronal image", "[sync]") {
    // Coronal target: rows run +x, columns run -z, plane y = 0, spanning
    // x in [0, 99], z in [100, 1] (100x100 px, 1 mm).
    PlaneRect coronal{plane({0, 0, 100}, {1, 0, 0}, {0, 0, -1}), 100, 100, "for"};
    // Axial slice at z = 40 covering x,y in [-50, 149].
    PlaneRect axial{plane({-50, -50, 40}, {1, 0, 0}, {0, 1, 0}), 200, 200, "for"};
    const auto line = referenceLine(axial, coronal);
    REQUIRE(line.has_value());
    // z = 40 -> row index 60 on the coronal image
    REQUIRE(line->first.y == Catch::Approx(60.0));
    REQUIRE(line->second.y == Catch::Approx(60.0));
    REQUIRE(std::abs(line->first.x - line->second.x) == Catch::Approx(200.0));
    // Parallel planes have no reference line
    PlaneRect axial2{plane({0, 0, 50}, {1, 0, 0}, {0, 1, 0}), 10, 10, "for"};
    REQUIRE_FALSE(referenceLine(axial2, axial).has_value());
}

TEST_CASE("Patient to pixel mapping inverts pixelToPatient", "[sync]") {
    const auto g = plane({10, -20, 5}, {0, 1, 0}, {0, 0, -1}, 0.7);
    const Vec3 p = g.pixelToPatient(12.5, 33.0);
    const auto px = patientToPixel(g, p);
    REQUIRE(px.x == Catch::Approx(12.5));
    REQUIRE(px.y == Catch::Approx(33.0));
}
