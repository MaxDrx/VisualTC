#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <numbers>
#include <random>

#include "measurements/MeasurementMath.h"
#include "measurements/RoiStatistics.h"

using namespace vtc;

TEST_CASE("QA de medidas: 100 px x 0.5 mm = 50 mm", "[measure]") {
    REQUIRE(distanceMm({10, 20}, {110, 20}, 0.5, 0.5) == Catch::Approx(50.0));
    REQUIRE(distanceMm({10, 20}, {10, 120}, 0.5, 0.5) == Catch::Approx(50.0));
    REQUIRE(distancePixels({0, 0}, {3, 4}) == Catch::Approx(5.0));
}

TEST_CASE("Anisotropic pixel spacing is honoured", "[measure]") {
    // sx (between columns) = 0.5, sy (between rows) = 2.0
    REQUIRE(distanceMm({0, 0}, {10, 0}, 0.5, 2.0) == Catch::Approx(5.0));
    REQUIRE(distanceMm({0, 0}, {0, 10}, 0.5, 2.0) == Catch::Approx(20.0));
    REQUIRE(distanceMm({0, 0}, {30, 4}, 0.5, 2.0) == Catch::Approx(std::hypot(15.0, 8.0)));
}

TEST_CASE("Angles are computed in physical space", "[measure]") {
    REQUIRE(angleDegrees({10, 0}, {0, 0}, {0, 10}, 1, 1) == Catch::Approx(90.0));
    REQUIRE(angleDegrees({10, 0}, {0, 0}, {-10, 0}, 1, 1) == Catch::Approx(180.0));
    REQUIRE(angleDegrees({10, 0}, {0, 0}, {10, 10}, 1, 1) == Catch::Approx(45.0));
    // 45 degrees in pixels becomes atan(2) with sy = 2 sx
    REQUIRE(angleDegrees({10, 0}, {0, 0}, {10, 10}, 1, 2) == Catch::Approx(std::atan(2.0) * 180 / std::numbers::pi));
    REQUIRE(cobbAngleDegrees({0, 0}, {10, 0}, {0, 0}, {10, 10}, 1, 1) == Catch::Approx(45.0));
    REQUIRE(cobbAngleDegrees({0, 0}, {10, 0}, {10, 10}, {0, 0}, 1, 1) == Catch::Approx(45.0));  // line direction irrelevant
}

TEST_CASE("Polygon area and perimeter", "[measure]") {
    const std::vector<Point2> sq{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    REQUIRE(polygonAreaMm2(sq, 0.5, 0.5) == Catch::Approx(25.0));
    REQUIRE(polygonPerimeterMm(sq, 0.5, 0.5) == Catch::Approx(20.0));
    REQUIRE(pointInPolygon(sq, {5, 5}));
    REQUIRE_FALSE(pointInPolygon(sq, {15, 5}));
    REQUIRE(ellipsePerimeterMm(10, 10) == Catch::Approx(2 * std::numbers::pi * 10).epsilon(1e-9));
}

namespace {
DecodedFrame rampFrame(int w, int h) {
    // value(x, y) = x + 100 * y, stored as int16 with slope 1, intercept -1000
    DecodedFrame f;
    f.width = w;
    f.height = h;
    f.format = PixelFormat::I16;
    f.data.resize(static_cast<size_t>(w * h * 2));
    auto* p = f.as<std::int16_t>();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            p[y * w + x] = static_cast<std::int16_t>(x + 100 * y);
        }
    }
    f.intercept = -1000.0;
    f.computeRange();
    return f;
}
}  // namespace

TEST_CASE("Rectangle ROI statistics in modality units", "[roi]") {
    const auto f = rampFrame(20, 20);
    std::vector<double> values;
    const auto s = rectangleStats(f, {2, 3}, {4, 5}, 0.5, 0.5, &values);
    // pixels x = 2..4, y = 3..5 -> 9 pixels
    REQUIRE(s.count == 9);
    REQUIRE(values.size() == 9);
    REQUIRE(s.min == Catch::Approx(2 + 300 - 1000));
    REQUIRE(s.max == Catch::Approx(4 + 500 - 1000));
    REQUIRE(s.mean == Catch::Approx(3 + 400 - 1000));
    // sample SD of {x} + 100{y}: var = var_x + 1e4 var_y, each var = 1 (n-1 = 8, 3x3 grid)
    const double expectedSd = std::sqrt((6 * 1.0 + 6 * 10000.0) / 8.0);
    REQUIRE(s.stdDev == Catch::Approx(expectedSd));
    REQUIRE(s.areaMm2 == Catch::Approx(1.0));  // 2 px * 0.5 mm squared
    REQUIRE(s.widthMm == Catch::Approx(1.0));
}

TEST_CASE("Ellipse ROI area and pixel inclusion", "[roi]") {
    const auto f = rampFrame(64, 64);
    const auto s = ellipseStats(f, {10, 10}, {30, 20}, 0.5, 1.0);
    REQUIRE(s.areaMm2 == Catch::Approx(std::numbers::pi * 5.0 * 5.0));  // a=10px*0.5, b=5px*1.0
    REQUIRE(s.count > 0);
    REQUIRE(std::abs(static_cast<double>(s.count) - std::numbers::pi * 10 * 5) < 20.0);
    REQUIRE(s.mean == Catch::Approx(20 + 1500 - 1000).margin(1.0));  // symmetric around the centre
}

TEST_CASE("ROI partially outside the image only uses valid pixels", "[roi]") {
    const auto f = rampFrame(10, 10);
    const auto s = rectangleStats(f, {-5, -5}, {1, 1}, 1, 1);
    REQUIRE(s.count == 4);
    REQUIRE(s.areaMm2 == Catch::Approx(36.0));
}

TEST_CASE("Polygon ROI and histogram", "[roi]") {
    const auto f = rampFrame(20, 20);
    std::vector<double> values;
    const auto s = polygonStats(f, {{2, 2}, {6, 2}, {6, 6}, {2, 6}}, 1, 1, &values);
    REQUIRE(s.areaMm2 == Catch::Approx(16.0));
    REQUIRE(s.perimeterMm == Catch::Approx(16.0));
    REQUIRE(s.count >= 9);
    const auto h = makeHistogram(values, 10);
    std::size_t total = 0;
    for (auto c : h.counts) {
        total += c;
    }
    REQUIRE(total == values.size());
    REQUIRE(h.min == Catch::Approx(s.min));
}

TEST_CASE("Color frames give geometry but no statistics", "[roi]") {
    DecodedFrame f;
    f.width = 4;
    f.height = 4;
    f.format = PixelFormat::RGB8;
    f.data.assign(48, 100);
    const auto s = rectangleStats(f, {0, 0}, {3, 3}, 1, 1);
    REQUIRE(s.count == 0);
    REQUIRE(std::isnan(s.mean));
    REQUIRE(s.areaMm2 == Catch::Approx(9.0));
}

TEST_CASE("Freehand ROI pixel membership matches the point-in-polygon rule", "[roi]") {
    // The scan-line implementation must select exactly the same pixels as the
    // reference even-odd test, including concave and self-crossing outlines.
    const DecodedFrame f = rampFrame(64, 48);
    std::mt19937 gen(5);
    std::uniform_real_distribution<double> coord(-8.0, 70.0);
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<Point2> pts;
        const int n = 3 + static_cast<int>(gen() % 12);
        for (int i = 0; i < n; ++i) {
            pts.push_back({coord(gen), coord(gen) * 0.7});
        }
        if (trial % 7 == 0) {
            pts.push_back({std::floor(pts[0].x), std::floor(pts[0].y)});  // vertices on pixel centres
        }
        std::vector<double> values;
        const auto s = polygonStats(f, pts, 1.0, 1.0, &values);
        std::size_t expected = 0;
        double sum = 0.0;
        for (int y = 0; y < f.height; ++y) {
            for (int x = 0; x < f.width; ++x) {
                if (pointInPolygon(pts, {static_cast<double>(x), static_cast<double>(y)})) {
                    ++expected;
                    sum += f.valueAt(x, y);
                }
            }
        }
        CAPTURE(trial, n);
        REQUIRE(s.count == expected);
        REQUIRE(values.size() == expected);
        if (expected > 0) {
            REQUIRE(s.mean == Catch::Approx(sum / static_cast<double>(expected)));
        }
    }
}
