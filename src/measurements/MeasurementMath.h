#pragma once

#include <vector>

namespace vtc {

// Continuous image coordinates: x = column, y = row; the centre of pixel
// (i, j) is at (i, j). Physical sizes use the frame's pixel spacing
// (sx between columns, sy between rows), so anisotropic pixels are handled.
struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

double distancePixels(Point2 a, Point2 b);
double distanceMm(Point2 a, Point2 b, double sx, double sy);

// Angle ABC (vertex at b) in degrees, computed in physical space. [0, 180]
double angleDegrees(Point2 a, Point2 b, Point2 c, double sx, double sy);

// Cobb angle between line (a1,a2) and line (b1,b2), in degrees. [0, 90]
double cobbAngleDegrees(Point2 a1, Point2 a2, Point2 b1, Point2 b2, double sx, double sy);

double polygonAreaMm2(const std::vector<Point2>& pts, double sx, double sy);
double polygonPerimeterMm(const std::vector<Point2>& pts, double sx, double sy, bool closed = true);
// Ramanujan approximation; semi-axes in mm.
double ellipsePerimeterMm(double a, double b);
bool pointInPolygon(const std::vector<Point2>& pts, Point2 p);

}  // namespace vtc
