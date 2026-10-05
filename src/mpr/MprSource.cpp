#include "mpr/MprSource.h"

#include <cstdio>

#include "app/Theme.h"

namespace vtc {

MprSource::MprSource(std::shared_ptr<MprSession> session, MprOrientation orientation, QObject* parent)
    : ImageSource(parent), session_(std::move(session)), orientation_(orientation) {
    connect(session_.get(), &MprSession::renderingChanged, this, [this] {
        cache_.reset();
        Q_EMIT contentChanged();
    });
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
    if (cache_ && cacheIndex_ == index && cacheVersion_ == session_->version()) {
        return cache_;
    }
    cache_ = reslice(session_->volume(), planeAt(index), session_->slab(), session_->interpolation());
    cacheIndex_ = index;
    cacheVersion_ = session_->version();
    return cache_;
}

FrameGeometry MprSource::geometryAt(int index) const {
    FrameGeometry g = planeAt(index).toFrameGeometry();
    const auto slab = session_->slab();
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
    if (session_->isOblique()) {
        label += tr(" (oblíquo)");
    }
    const auto slab = session_->slab();
    if (slab.thickness > 0.0) {
        const char* mode = slab.mode == SlabMode::MIP ? "MIP" : (slab.mode == SlabMode::MinIP ? "MinIP" : "Média");
        label += QStringLiteral(" · %1 %2 mm").arg(QString::fromLatin1(mode)).arg(slab.thickness);
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

Point2 MprSource::crosshairPixel(int index) const {
    const ReslicePlane p = planeAt(index);
    const Vec3 rel = session_->center() - p.origin;
    return {rel.dot(p.u) / p.spacingU, rel.dot(p.v) / p.spacingV};
}

std::vector<GuideLine> MprSource::crosshairLines(int index) const {
    std::vector<GuideLine> lines;
    const ReslicePlane p = planeAt(index);
    const Point2 c = crosshairPixel(index);
    const double reach = 4.0 * std::max(p.width, p.height);
    const Vec3 n = p.normal();
    for (auto other : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
        if (other == orientation_) {
            continue;
        }
        const Vec3 dir = n.cross(session_->view(other).n);
        if (dir.norm() < 1e-6) {
            continue;  // parallel planes: no intersection line
        }
        const Vec3 d = dir.normalized();
        double dx = d.dot(p.u) / p.spacingU;
        double dy = d.dot(p.v) / p.spacingV;
        const double len = std::hypot(dx, dy);
        dx /= len;
        dy /= len;
        lines.push_back({{c.x - dx * reach, c.y - dy * reach}, {c.x + dx * reach, c.y + dy * reach}, colorFor(other),
                         false});
    }
    return lines;
}

}  // namespace vtc
