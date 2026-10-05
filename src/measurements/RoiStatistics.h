#pragma once

#include <cstddef>
#include <limits>
#include <vector>

#include "imaging/PixelData.h"
#include "measurements/MeasurementMath.h"

namespace vtc {

// Statistics of modality values (HU for CT) of the pixels whose centres lie
// inside a region. Geometric sizes are of the drawn shape itself.
struct RoiStatistics {
    std::size_t count = 0;
    double mean = std::numeric_limits<double>::quiet_NaN();
    double stdDev = std::numeric_limits<double>::quiet_NaN();  // sample SD (n-1)
    double min = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
    double areaPx = 0.0;
    double areaMm2 = 0.0;
    double perimeterMm = 0.0;
    double widthMm = 0.0;
    double heightMm = 0.0;
};

RoiStatistics rectangleStats(const DecodedFrame& frame, Point2 p0, Point2 p1, double sx, double sy,
                             std::vector<double>* values = nullptr);
RoiStatistics ellipseStats(const DecodedFrame& frame, Point2 p0, Point2 p1, double sx, double sy,
                           std::vector<double>* values = nullptr);
RoiStatistics polygonStats(const DecodedFrame& frame, const std::vector<Point2>& pts, double sx, double sy,
                           std::vector<double>* values = nullptr);

struct Histogram {
    double min = 0.0;
    double binWidth = 1.0;
    std::vector<std::size_t> counts;
    [[nodiscard]] std::size_t maxCount() const;
};

Histogram makeHistogram(const std::vector<double>& values, int bins);

}  // namespace vtc
