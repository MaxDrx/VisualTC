#include "synchronization/SpatialSync.h"

#include <cmath>
#include <limits>
#include <numbers>

#include "dicom/DicomGeometry.h"

namespace vtc {

std::vector<Vec3> PlaneRect::corners() const {
    const double x1 = width - 0.5;
    const double y1 = height - 0.5;
    return {geometry.pixelToPatient(-0.5, -0.5), geometry.pixelToPatient(x1, -0.5), geometry.pixelToPatient(x1, y1),
            geometry.pixelToPatient(-0.5, y1)};
}

bool spatiallyComparable(const PlaneRect& a, const PlaneRect& b) {
    return a.geometry.isSpatial() && b.geometry.isSpatial() && !a.frameOfReferenceUid.empty() &&
           a.frameOfReferenceUid == b.frameOfReferenceUid;
}

bool planesParallel(const Vec3& n1, const Vec3& n2, double maxAngleDegrees) {
    const double c = std::abs(n1.normalized().dot(n2.normalized()));
    return c >= std::cos(maxAngleDegrees * std::numbers::pi / 180.0);
}

std::optional<int> nearestSliceIndex(const std::vector<FrameGeometry>& frames, const Vec3& point, double tolerance) {
    int best = -1;
    double bestDist = std::numeric_limits<double>::infinity();
    for (int i = 0; i < static_cast<int>(frames.size()); ++i) {
        const auto& g = frames[static_cast<size_t>(i)];
        if (!g.hasPosition || !g.hasOrientation) {
            continue;
        }
        const double d = std::abs((point - g.position).dot(g.normal()));
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    if (best < 0 || bestDist > tolerance) {
        return std::nullopt;
    }
    return best;
}

Point2 patientToPixel(const FrameGeometry& g, const Vec3& p) {
    const Vec3 rel = p - g.position;
    return {rel.dot(g.rowDir) / g.spacingX, rel.dot(g.colDir) / g.spacingY};
}

std::optional<std::pair<Point2, Point2>> referenceLine(const PlaneRect& other, const PlaneRect& target) {
    const Vec3 nt = target.geometry.normal();
    const Vec3 no = other.geometry.normal();
    if (planesParallel(nt, no, 1.0)) {
        return std::nullopt;
    }
    const auto c = other.corners();
    std::vector<Vec3> hits;
    for (size_t i = 0; i < c.size(); ++i) {
        Vec3 p;
        if (intersectSegmentWithPlane(c[i], c[(i + 1) % c.size()], target.geometry.position, nt, p)) {
            bool dup = false;
            for (const auto& h : hits) {
                if (distance(h, p) < 1e-6) {
                    dup = true;
                }
            }
            if (!dup) {
                hits.push_back(p);
            }
        }
    }
    if (hits.size() < 2) {
        return std::nullopt;
    }
    // Farthest pair (a plane can touch a corner and produce >2 hits).
    size_t ia = 0;
    size_t ib = 1;
    double best = -1.0;
    for (size_t i = 0; i < hits.size(); ++i) {
        for (size_t j = i + 1; j < hits.size(); ++j) {
            const double d = distance(hits[i], hits[j]);
            if (d > best) {
                best = d;
                ia = i;
                ib = j;
            }
        }
    }
    return std::make_pair(patientToPixel(target.geometry, hits[ia]), patientToPixel(target.geometry, hits[ib]));
}

}  // namespace vtc
