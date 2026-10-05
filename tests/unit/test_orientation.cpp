#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "imaging/Orientation.h"

using namespace vtc;

TEST_CASE("Axial image orientation markers (radiological convention)", "[orientation]") {
    // IOP 1\0\0\0\1\0: rows run to patient left, columns to posterior.
    const Vec3 row{1, 0, 0};
    const Vec3 col{0, 1, 0};
    REQUIRE(orientationLetters(row) == "L");    // right edge of the screen
    REQUIRE(orientationLetters(-row) == "R");   // left edge
    REQUIRE(orientationLetters(-col) == "A");   // top edge
    REQUIRE(orientationLetters(col) == "P");    // bottom edge
    REQUIRE(planeLabel(row.cross(col)) == "Axial");
}

TEST_CASE("Coronal and sagittal orientation markers", "[orientation]") {
    // Coronal: 1\0\0\0\0\-1
    REQUIRE(orientationLetters({0, 0, -1}) == "F");
    REQUIRE(orientationLetters({0, 0, 1}) == "H");
    REQUIRE(planeLabel(Vec3{1, 0, 0}.cross(Vec3{0, 0, -1})) == "Coronal");
    // Sagittal: 0\1\0\0\0\-1
    REQUIRE(orientationLetters({0, 1, 0}) == "P");
    REQUIRE(planeLabel(Vec3{0, 1, 0}.cross(Vec3{0, 0, -1})) == "Sagital");
}

TEST_CASE("Oblique directions list components by magnitude", "[orientation]") {
    const double c = std::cos(0.5);
    const double s = std::sin(0.5);
    REQUIRE(orientationLetters({c, s, 0}) == "LP");
    REQUIRE(orientationLetters({-s, -c, 0}) == "AR");
    // 5 degrees off-axis: only the dominant letter
    REQUIRE(orientationLetters({std::cos(0.087), 0, std::sin(0.087)}) == "L");
    REQUIRE(planeLabel({0.6, 0.0, 0.8}) == "Axial");
    REQUIRE(planeLabel({0.7, 0.0, 0.7}) == "Oblíquo");
}
