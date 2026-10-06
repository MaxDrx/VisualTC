#include "mpr/MprSource.h"

#include <QLocale>
#include <cmath>
#include <cstdio>

#include "app/Theme.h"

namespace vtc {

MprSource::MprSource(std::shared_ptr<MprSession> session, MprOrientation orientation, QObject* parent)
    : ImageSource(parent), session_(std::move(session)), orientation_(orientation) {
    connect(session_.get(), &MprSession::renderingChanged, this, [this] {
        if (cache_ && cacheVersion_ == session_->version(orientation_)) {
            return;  // this plane did not change (or is already recomputed)
        }
        cache_.reset();
        Q_EMIT contentChanged();
    });
}

QString MprSource::slabModeName(SlabMode m) {
    switch (m) {
        case SlabMode::MIP: return QStringLiteral("MIP");
        case SlabMode::MinIP: return QStringLiteral("MinIP");
        case SlabMode::Average: return tr("Média");
    }
    return {};
}

QColor MprSource::colorFor(MprOrientation o) {
    switch (o) {
        case MprOrientation::Axial: return Theme::axialColor();
        case MprOrientation::Coronal: return Theme::coronalColor();
        case MprOrientation::Sagittal: return Theme::sagittalColor();
    }
    return Qt::white;
}

int MprSource::count() const { return session_->view(orientation_).sliceCount; }

ReslicePlane MprSource::planeAt(int index) const {
    const MprView& v = session_->view(orientation_);
    return v.planeThrough(v.pointAtSlice(session_->center(), index));
}

DecodedFramePtr MprSource::image(int index) {
    const int version = session_->version(orientation_);
    if (cache_ && cacheIndex_ == index && cacheVersion_ == version) {
        return cache_;
    }
    cache_ = reslice(session_->volume(), planeAt(index), session_->slab(orientation_), session_->interpolation());
    cacheIndex_ = index;
    cacheVersion_ = version;
    return cache_;
}

FrameGeometry MprSource::geometryAt(int index) const {
    FrameGeometry g = planeAt(index).toFrameGeometry();
    const auto slab = session_->slab(orientation_);
    if (slab.thickness > 0.0) {
        g.sliceThickness = slab.thickness;
    }
    return g;
}

QSize MprSource::sizeAt(int /*index*/) const {
    const MprView& v = session_->view(orientation_);
    return {v.width, v.height};
}

std::string MprSource::frameOfReference() const { return session_->volume().frameOfReferenceUid(); }

const InstanceInfo* MprSource::instanceAt(int /*index*/) const {
    const auto& s = session_->series();
    return s && !s->frames.empty() ? s->frames.front().instance.get() : nullptr;
}

const FrameInfo* MprSource::frameInfoAt(int /*index*/) const {
    const auto& s = session_->series();
    return s && !s->frames.empty() ? &s->frames[s->frames.size() / 2].info() : nullptr;
}

QString MprSource::seriesLabel() const {
    QString label = "MPR " + QString::fromStdString(toLabel(orientation_));
    if (session_->isOblique(orientation_)) {
        label += tr(" (oblíquo)");
    }
    const auto slab = session_->slab(orientation_);
    if (slab.thickness > 0.0) {
        label += QStringLiteral(" · %1 %2 mm").arg(slabModeName(slab.mode), QLocale().toString(slab.thickness, 'f', 1));
    }
    const auto& s = session_->series();
    if (s) {
        label += " · " + QString::fromStdString(s->description());
    }
    return label;
}

std::string MprSource::stateKey() const {
    return "mpr:" + (session_->series() ? session_->series()->id : std::string()) + ":" + toLabel(orientation_);
}

std::string MprSource::annotationKey(int index) const {
    const ReslicePlane p = planeAt(index);
    char buf[160];
    (void)std::snprintf(buf, sizeof(buf), "mpr:%d:%.3f,%.3f,%.3f:%.4f,%.4f,%.4f:%.4f,%.4f,%.4f", static_cast<int>(orientation_),
                  p.origin.x, p.origin.y, p.origin.z, p.u.x, p.u.y, p.u.z, p.v.x, p.v.y, p.v.z);
    return (session_->series() ? session_->series()->id : std::string()) + buf;
}

bool MprSource::isCt() const { return session_->volume().isCt(); }

int MprSource::initialIndex() const { return session_->view(orientation_).sliceIndexOf(session_->center()); }

std::optional<QSizeF> MprSource::fitExtentMm() const {
    // An oblique plane's bounding box grows and shrinks as it turns; framing
    // the straight view's extent keeps the scale steady while the user rotates.
    const MprView& v = session_->orthogonalView(orientation_);
    return QSizeF(v.width * v.spacingU, v.height * v.spacingV);
}

Point2 MprSource::crosshairPixel(int index) const {
    const ReslicePlane p = planeAt(index);
    const Vec3 rel = session_->center() - p.origin;
    return {rel.dot(p.u) / p.spacingU, rel.dot(p.v) / p.spacingV};
}

std::vector<MprGuide> MprSource::guides(int index) const {
    std::vector<MprGuide> out;
    const ReslicePlane p = planeAt(index);
    const Vec3 n = p.normal();
    auto toPixels = [&p](const Vec3& d) { return Point2{d.dot(p.u) / p.spacingU, d.dot(p.v) / p.spacingV}; };
    for (auto other : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
        if (other == orientation_) {
            continue;
        }
        const Vec3 np = session_->view(other).n;
        const Vec3 dir = n.cross(np);
        if (dir.norm() < 1e-6) {
            continue;  // parallel planes: no intersection line
        }
        const Vec3 d = dir.normalized();
        // In-plane direction across the line; the slab boundary (other plane
        // moved by t/2 along its normal) crosses this view at e * (t/2) / (e . np).
        Vec3 e = n.cross(d);
        double en = e.dot(np);
        if (std::abs(en) < 1e-6) {
            continue;
        }
        if (en < 0.0) {
            e = -e;
            en = -en;
        }
        MprGuide g;
        g.plane = static_cast<int>(other);
        g.direction = toPixels(d);
        g.mmOffset = toPixels(e / en);
        const SlabParams slab = session_->slab(other);
        g.thickness = slab.thickness;
        g.slabMode = slabModeName(slab.mode);
        g.color = colorFor(other);
        out.push_back(g);
    }
    return out;
}

}  // namespace vtc
