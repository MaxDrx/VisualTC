#include "measurements/RoiStatistics.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vtc {

namespace {

class Accumulator {
public:
    void add(double v) {
        if (std::isnan(v)) {
            return;
        }
        ++n_;
        const double delta = v - mean_;
        mean_ += delta / static_cast<double>(n_);
        m2_ += delta * (v - mean_);
        min_ = n_ == 1 ? v : std::min(min_, v);
        max_ = n_ == 1 ? v : std::max(max_, v);
    }
    void fill(RoiStatistics& s) const {
        s.count = n_;
        if (n_ == 0) {
            return;
        }
        s.mean = mean_;
        s.min = min_;
        s.max = max_;
        s.stdDev = n_ > 1 ? std::sqrt(m2_ / static_cast<double>(n_ - 1)) : 0.0;
    }

private:
    std::size_t n_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;
    double min_ = 0.0;
    double max_ = 0.0;
};

template <class Inside>
void scan(const DecodedFrame& frame, double x0, double y0, double x1, double y1, Inside inside, RoiStatistics& s,
          std::vector<double>* values) {
    Accumulator acc;
    if (!frame.isColor()) {
        const int i0 = std::max(0, static_cast<int>(std::ceil(std::min(x0, x1))));
        const int i1 = std::min(frame.width - 1, static_cast<int>(std::floor(std::max(x0, x1))));
        const int j0 = std::max(0, static_cast<int>(std::ceil(std::min(y0, y1))));
        const int j1 = std::min(frame.height - 1, static_cast<int>(std::floor(std::max(y0, y1))));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                if (inside(static_cast<double>(i), static_cast<double>(j))) {
                    const double v = frame.valueAt(i, j);
                    acc.add(v);
                    if (values != nullptr && !std::isnan(v)) {
                        values->push_back(v);
                    }
                }
            }
        }
    }
    acc.fill(s);
}

// Pixels whose centres lie inside the polygon, by scan line: for each image
// row the edge crossings are computed once and sorted, which selects exactly
// the pixels accepted by pointInPolygon() (even-odd rule, crossing strictly
// to the right of the centre) at O(rows x edges + pixels) instead of
// O(pixels x edges): large freehand ROIs stay interactive.
void scanPolygon(const DecodedFrame& frame, const std::vector<Point2>& pts, double x0, double y0, double x1,
                 double y1, RoiStatistics& s, std::vector<double>* values) {
    Accumulator acc;
    if (!frame.isColor() && pts.size() >= 3) {
        const int i0 = std::max(0, static_cast<int>(std::ceil(x0)));
        const int i1 = std::min(frame.width - 1, static_cast<int>(std::floor(x1)));
        const int j0 = std::max(0, static_cast<int>(std::ceil(y0)));
        const int j1 = std::min(frame.height - 1, static_cast<int>(std::floor(y1)));
        std::vector<double> crossings;
        const std::size_t n = pts.size();
        for (int j = j0; j <= j1; ++j) {
            const double py = static_cast<double>(j);
            crossings.clear();
            for (std::size_t i = 0, k = n - 1; i < n; k = i++) {
                const Point2& a = pts[i];
                const Point2& b = pts[k];
                if ((a.y > py) != (b.y > py)) {
                    crossings.push_back((b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x);
                }
            }
            if (crossings.empty()) {
                continue;
            }
            std::sort(crossings.begin(), crossings.end());
            // inside(x) <=> odd number of crossings c with x < c
            std::size_t notRight = 0;  // crossings c <= x
            for (int i = i0; i <= i1; ++i) {
                const double px = static_cast<double>(i);
                while (notRight < crossings.size() && crossings[notRight] <= px) {
                    ++notRight;
                }
                if (((crossings.size() - notRight) & 1u) != 0) {
                    const double v = frame.valueAt(i, j);
                    acc.add(v);
                    if (values != nullptr && !std::isnan(v)) {
                        values->push_back(v);
                    }
                }
            }
        }
    }
    acc.fill(s);
}

}  // namespace

RoiStatistics rectangleStats(const DecodedFrame& frame, Point2 p0, Point2 p1, double sx, double sy,
                             std::vector<double>* values) {
    RoiStatistics s;
    const double w = std::abs(p1.x - p0.x);
    const double h = std::abs(p1.y - p0.y);
    s.areaPx = w * h;
    s.widthMm = w * sx;
    s.heightMm = h * sy;
    s.areaMm2 = s.widthMm * s.heightMm;
    s.perimeterMm = 2.0 * (s.widthMm + s.heightMm);
    scan(frame, p0.x, p0.y, p1.x, p1.y, [](double, double) { return true; }, s, values);
    return s;
}

RoiStatistics ellipseStats(const DecodedFrame& frame, Point2 p0, Point2 p1, double sx, double sy,
                           std::vector<double>* values) {
    RoiStatistics s;
    const double a = std::abs(p1.x - p0.x) / 2.0;
    const double b = std::abs(p1.y - p0.y) / 2.0;
    const double cx = (p0.x + p1.x) / 2.0;
    const double cy = (p0.y + p1.y) / 2.0;
    s.areaPx = std::numbers::pi * a * b;
    s.widthMm = 2.0 * a * sx;
    s.heightMm = 2.0 * b * sy;
    s.areaMm2 = std::numbers::pi * (a * sx) * (b * sy);
    s.perimeterMm = ellipsePerimeterMm(a * sx, b * sy);
    if (a <= 0.0 || b <= 0.0) {
        return s;
    }
    scan(
        frame, p0.x, p0.y, p1.x, p1.y,
        [=](double x, double y) {
            const double dx = (x - cx) / a;
            const double dy = (y - cy) / b;
            return dx * dx + dy * dy <= 1.0;
        },
        s, values);
    return s;
}

RoiStatistics polygonStats(const DecodedFrame& frame, const std::vector<Point2>& pts, double sx, double sy,
                           std::vector<double>* values) {
    RoiStatistics s;
    if (pts.size() < 3) {
        return s;
    }
    s.areaPx = polygonAreaMm2(pts, 1.0, 1.0);
    s.areaMm2 = polygonAreaMm2(pts, sx, sy);
    s.perimeterMm = polygonPerimeterMm(pts, sx, sy, true);
    double x0 = pts[0].x;
    double x1 = pts[0].x;
    double y0 = pts[0].y;
    double y1 = pts[0].y;
    for (const auto& p : pts) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    s.widthMm = (x1 - x0) * sx;
    s.heightMm = (y1 - y0) * sy;
    scanPolygon(frame, pts, x0, y0, x1, y1, s, values);
    return s;
}

std::size_t Histogram::maxCount() const {
    return counts.empty() ? 0 : *std::max_element(counts.begin(), counts.end());
}

Histogram makeHistogram(const std::vector<double>& values, int bins) {
    Histogram h;
    if (values.empty() || bins <= 0) {
        return h;
    }
    const auto [mnIt, mxIt] = std::minmax_element(values.begin(), values.end());
    const double mn = *mnIt;
    const double mx = *mxIt;
    h.min = mn;
    h.counts.assign(static_cast<std::size_t>(bins), 0);
    if (mx <= mn) {
        h.binWidth = 1.0;
        h.counts[0] = values.size();
        return h;
    }
    h.binWidth = (mx - mn) / bins;
    for (double v : values) {
        auto idx = static_cast<std::size_t>((v - mn) / h.binWidth);
        idx = std::min(idx, static_cast<std::size_t>(bins - 1));
        ++h.counts[idx];
    }
    return h;
}

}  // namespace vtc
