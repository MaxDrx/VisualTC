#include "mpr/MprSession.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vtc {

namespace {
constexpr MprOrientation kAll[3] = {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal};
constexpr double kMaxSlab = 500.0;  // mm
}  // namespace

MprSession::MprSession(VolumePtr volume, SeriesPtr series, QObject* parent)
    : QObject(parent), volume_(std::move(volume)), series_(std::move(series)) {
    for (auto o : kAll) {
        ortho_[idx(o)] = makeOrthogonalView(*volume_, o);
    }
    resetOrientation();
    center_ = volume_->center();
}

void MprSession::resetOrientation() {
    for (auto o : kAll) {
        views_[idx(o)] = ortho_[idx(o)];
        oblique_[idx(o)] = false;
        ++versions_[idx(o)];
    }
    Q_EMIT renderingChanged();
}

Vec3 MprSession::clampToVolume(const Vec3& p) const {
    // Keep the crosshair inside the volume's bounding box in the straight
    // axial frame (the same box whatever the planes' rotation).
    const auto& v = ortho_[0];
    const double u = std::clamp(p.dot(v.u), v.uMin, v.uMax);
    const double w = std::clamp(p.dot(v.v), v.vMin, v.vMax);
    const double n = std::clamp(p.dot(v.n), v.nMin, v.nMax);
    return v.u * u + v.v * w + v.n * n;
}

void MprSession::setCenter(const Vec3& p) {
    const Vec3 c = clampToVolume(p);
    if (distance(c, center_) < 1e-9) {
        return;
    }
    center_ = c;
    Q_EMIT centerChanged();
}

void MprSession::moveToSlice(MprOrientation o, int index) {
    setCenter(view(o).pointAtSlice(center_, index));
}

void MprSession::setSlab(const SlabParams& s) {
    const double t = std::clamp(s.thickness, 0.0, kMaxSlab);
    bool changed = false;
    for (auto o : kAll) {
        const bool modeMatters = thickness_[idx(o)] > 0.0 || t > 0.0;
        if (thickness_[idx(o)] != t || (s.mode != slabMode_ && modeMatters)) {
            thickness_[idx(o)] = t;
            ++versions_[idx(o)];
            changed = true;
        }
    }
    slabMode_ = s.mode;
    if (changed) {
        Q_EMIT renderingChanged();
    }
}

void MprSession::setSlabThickness(MprOrientation o, double mm) {
    const double t = std::clamp(mm, 0.0, kMaxSlab);
    if (std::abs(thickness_[idx(o)] - t) < 1e-9) {
        return;
    }
    thickness_[idx(o)] = t;
    ++versions_[idx(o)];
    Q_EMIT renderingChanged();
}

void MprSession::setSlabMode(SlabMode mode) {
    if (mode == slabMode_) {
        return;
    }
    slabMode_ = mode;
    bool changed = false;
    for (auto o : kAll) {
        if (thickness_[idx(o)] > 0.0) {  // a thin plane looks the same in every mode
            ++versions_[idx(o)];
            changed = true;
        }
    }
    if (changed) {
        Q_EMIT renderingChanged();
    }
}

void MprSession::setInterpolation(Interpolation i) {
    if (i == interp_) {
        return;
    }
    interp_ = i;
    for (auto o : kAll) {
        ++versions_[idx(o)];
    }
    Q_EMIT renderingChanged();
}

void MprSession::rotateView(MprOrientation o, const Vec3& axis, double radians) {
    const MprView& cur = view(o);
    const Vec3 u = rotateAroundAxis(cur.u, axis, radians).normalized();
    // Re-orthogonalize: many small rotations must not let the frame drift.
    Vec3 v = rotateAroundAxis(cur.v, axis, radians);
    v = (v - u * u.dot(v)).normalized();
    views_[idx(o)] = makeView(*volume_, o, u, v);
    oblique_[idx(o)] = true;
    ++versions_[idx(o)];
}

void MprSession::rotate(MprOrientation o, MprOrientation around, double degrees) {
    if (o == around || !std::isfinite(degrees) || degrees == 0.0) {
        return;
    }
    rotateView(o, view(around).n, degrees * std::numbers::pi / 180.0);
    Q_EMIT renderingChanged();
}

void MprSession::rotateOthers(MprOrientation around, double degrees) {
    if (!std::isfinite(degrees) || degrees == 0.0) {
        return;
    }
    const Vec3 axis = view(around).n;
    const double a = degrees * std::numbers::pi / 180.0;
    for (auto o : kAll) {
        if (o != around) {
            rotateView(o, axis, a);
        }
    }
    Q_EMIT renderingChanged();
}

}  // namespace vtc
