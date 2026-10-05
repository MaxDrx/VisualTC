#include "mpr/MprGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vtc {

std::string toLabel(MprOrientation o) {
    switch (o) {
        case MprOrientation::Axial: return "Axial";
        case MprOrientation::Coronal: return "Coronal";
        case MprOrientation::Sagittal: return "Sagital";
    }
    return {};
}

ReslicePlane MprView::planeThrough(const Vec3& point) const {
    ReslicePlane p;
    p.u = u;
    p.v = v;
    p.spacingU = spacingU;
    p.spacingV = spacingV;
    p.width = width;
    p.height = height;
    p.origin = u * uMin + v * vMin + n * point.dot(n);
    return p;
}

int MprView::sliceIndexOf(const Vec3& point) const {
    const int idx = static_cast<int>(std::lround((point.dot(n) - nMin) / step));
    return std::clamp(idx, 0, std::max(0, sliceCount - 1));
}

Vec3 MprView::pointAtSlice(const Vec3& point, int index) const {
    index = std::clamp(index, 0, std::max(0, sliceCount - 1));
    const double target = nMin + index * step;
    return point + n * (target - point.dot(n));
}

namespace {
struct Axis {
    Vec3 dir;
    double spacing = 0.0;
};

// Spacing along an arbitrary unit direction: exact when aligned with a
// volume axis, otherwise the finest voxel dimension.
double spacingAlong(const Vec3& d, const std::array<Axis, 3>& axes) {
    for (const auto& a : axes) {
        if (std::abs(a.dir.dot(d)) > 0.9999) {
            return a.spacing;
        }
    }
    return std::min({axes[0].spacing, axes[1].spacing, axes[2].spacing});
}
}  // namespace

MprView makeView(const ImageVolume& vol, MprOrientation orientation, const Vec3& u, const Vec3& v) {
    MprView view;
    view.orientation = orientation;
    view.u = u.normalized();
    view.v = v.normalized();
    view.n = view.u.cross(view.v).normalized();
    const std::array<Axis, 3> axes{{{vol.rowDir(), vol.spacingX()},
                                    {vol.colDir(), vol.spacingY()},
                                    {vol.normal(), vol.sliceSpacing()}}};
    view.spacingU = spacingAlong(view.u, axes);
    view.spacingV = spacingAlong(view.v, axes);
    view.step = spacingAlong(view.n, axes);

    view.uMin = view.vMin = view.nMin = std::numeric_limits<double>::infinity();
    view.uMax = view.vMax = view.nMax = -std::numeric_limits<double>::infinity();
    for (const Vec3& c : vol.corners()) {
        const double cu = c.dot(view.u);
        const double cv = c.dot(view.v);
        const double cn = c.dot(view.n);
        view.uMin = std::min(view.uMin, cu);
        view.uMax = std::max(view.uMax, cu);
        view.vMin = std::min(view.vMin, cv);
        view.vMax = std::max(view.vMax, cv);
        view.nMin = std::min(view.nMin, cn);
        view.nMax = std::max(view.nMax, cn);
    }
    view.width = std::max(1, static_cast<int>(std::lround((view.uMax - view.uMin) / view.spacingU)) + 1);
    view.height = std::max(1, static_cast<int>(std::lround((view.vMax - view.vMin) / view.spacingV)) + 1);
    view.sliceCount = std::max(1, static_cast<int>(std::lround((view.nMax - view.nMin) / view.step)) + 1);
    return view;
}

MprView makeOrthogonalView(const ImageVolume& vol, MprOrientation orientation) {
    // Canonical screen axes (radiological convention, LPS):
    //   axial:    right = patient left (+x), down = posterior (+y)
    //   coronal:  right = patient left (+x), down = feet (-z)
    //   sagittal: right = posterior (+y),    down = feet (-z)
    Vec3 cu{1, 0, 0};
    Vec3 cv{0, 1, 0};
    if (orientation == MprOrientation::Coronal) {
        cv = {0, 0, -1};
    } else if (orientation == MprOrientation::Sagittal) {
        cu = {0, 1, 0};
        cv = {0, 0, -1};
    }
    const std::array<Vec3, 3> axes{vol.rowDir(), vol.colDir(), vol.normal()};
    auto bestAxis = [&](const Vec3& target, int exclude) {
        int best = -1;
        double bestDot = -1.0;
        for (int i = 0; i < 3; ++i) {
            if (i == exclude) {
                continue;
            }
            const double d = std::abs(axes[static_cast<size_t>(i)].dot(target));
            if (d > bestDot) {
                bestDot = d;
                best = i;
            }
        }
        return best;
    };
    const int iu = bestAxis(cu, -1);
    const int iv = bestAxis(cv, iu);
    Vec3 u = axes[static_cast<size_t>(iu)];
    Vec3 v = axes[static_cast<size_t>(iv)];
    if (u.dot(cu) < 0.0) {
        u = -u;
    }
    if (v.dot(cv) < 0.0) {
        v = -v;
    }
    return makeView(vol, orientation, u, v);
}

}  // namespace vtc
