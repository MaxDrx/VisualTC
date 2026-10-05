#include "mpr/MprSession.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vtc {

MprSession::MprSession(VolumePtr volume, SeriesPtr series, QObject* parent)
    : QObject(parent), volume_(std::move(volume)), series_(std::move(series)) {
    resetOrientation();
    center_ = volume_->center();
}

void MprSession::resetOrientation() {
    for (auto o : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
        views_[static_cast<size_t>(o)] = makeOrthogonalView(*volume_, o);
    }
    oblique_ = false;
    ++version_;
    Q_EMIT renderingChanged();
}

Vec3 MprSession::clampToVolume(const Vec3& p) const {
    // Keep the crosshair inside the volume's bounding box in the axial frame.
    const auto& v = views_[0];
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
    slab_ = s;
    ++version_;
    Q_EMIT renderingChanged();
}

void MprSession::setInterpolation(Interpolation i) {
    interp_ = i;
    ++version_;
    Q_EMIT renderingChanged();
}

void MprSession::rotate(MprOrientation o, MprOrientation around, double degrees) {
    if (o == around) {
        return;
    }
    const Vec3 axis = view(around).n;
    const double a = degrees * std::numbers::pi / 180.0;
    const MprView& cur = view(o);
    const Vec3 u = rotateAroundAxis(cur.u, axis, a).normalized();
    const Vec3 v = rotateAroundAxis(cur.v, axis, a).normalized();
    views_[static_cast<size_t>(o)] = makeView(*volume_, o, u, v);
    oblique_ = true;
    ++version_;
    Q_EMIT renderingChanged();
}

}  // namespace vtc
