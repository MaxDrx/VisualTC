#include "measurements/MeasurementMath.h"

#include <cmath>
#include <numbers>

namespace vtc {

double distancePixels(Point2 a, Point2 b) { return std::hypot(b.x - a.x, b.y - a.y); }

double distanceMm(Point2 a, Point2 b, double sx, double sy) {
    return std::hypot((b.x - a.x) * sx, (b.y - a.y) * sy);
}

double angleDegrees(Point2 a, Point2 b, Point2 c, double sx, double sy) {
    const double ux = (a.x - b.x) * sx;
    const double uy = (a.y - b.y) * sy;
    const double vx = (c.x - b.x) * sx;
    const double vy = (c.y - b.y) * sy;
    const double nu = std::hypot(ux, uy);
    const double nv = std::hypot(vx, vy);
    if (nu == 0.0 || nv == 0.0) {
        return 0.0;
    }
    // atan2 of cross/dot is numerically stable near 0 and 180 degrees.
    const double cross = ux * vy - uy * vx;
    const double dot = ux * vx + uy * vy;
    return std::abs(std::atan2(cross, dot)) * 180.0 / std::numbers::pi;
}

double cobbAngleDegrees(Point2 a1, Point2 a2, Point2 b1, Point2 b2, double sx, double sy) {
    const double ux = (a2.x - a1.x) * sx;
    const double uy = (a2.y - a1.y) * sy;
    const double vx = (b2.x - b1.x) * sx;
    const double vy = (b2.y - b1.y) * sy;
    const double nu = std::hypot(ux, uy);
    const double nv = std::hypot(vx, vy);
    if (nu == 0.0 || nv == 0.0) {
        return 0.0;
    }
    double ang = std::abs(std::atan2(ux * vy - uy * vx, ux * vx + uy * vy)) * 180.0 / std::numbers::pi;
    if (ang > 90.0) {
        ang = 180.0 - ang;  // lines, not vectors
    }
    return ang;
}

double polygonAreaMm2(const std::vector<Point2>& pts, double sx, double sy) {
    if (pts.size() < 3) {
        return 0.0;
    }
    double acc = 0.0;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Point2& p = pts[i];
        const Point2& q = pts[(i + 1) % pts.size()];
        acc += p.x * q.y - q.x * p.y;
    }
    return std::abs(acc) * 0.5 * sx * sy;
}

double polygonPerimeterMm(const std::vector<Point2>& pts, double sx, double sy, bool closed) {
    if (pts.size() < 2) {
        return 0.0;
    }
    double acc = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        acc += distanceMm(pts[i - 1], pts[i], sx, sy);
    }
    if (closed) {
        acc += distanceMm(pts.back(), pts.front(), sx, sy);
    }
    return acc;
}

double ellipsePerimeterMm(double a, double b) {
    a = std::abs(a);
    b = std::abs(b);
    return std::numbers::pi * (3.0 * (a + b) - std::sqrt((3.0 * a + b) * (a + 3.0 * b)));
}

bool pointInPolygon(const std::vector<Point2>& pts, Point2 p) {
    bool inside = false;
    const size_t n = pts.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point2& a = pts[i];
        const Point2& b = pts[j];
        if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

}  // namespace vtc
