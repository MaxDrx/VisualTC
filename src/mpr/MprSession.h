#pragma once

#include <QObject>
#include <array>
#include <memory>

#include "dicom/DicomSeries.h"
#include "mpr/ImageVolume.h"
#include "mpr/MprGeometry.h"
#include "mpr/Reslicer.h"

namespace vtc {

// Shared state of one multiplanar reconstruction: the volume, the three
// viewing frames and the crosshair point (patient coordinates) where the
// three planes intersect. Every MPR viewport derives its plane from here, so
// moving the crosshair in one view updates the others.
class MprSession : public QObject {
    Q_OBJECT
public:
    MprSession(VolumePtr volume, SeriesPtr series, QObject* parent = nullptr);

    [[nodiscard]] const ImageVolume& volume() const { return *volume_; }
    [[nodiscard]] const SeriesPtr& series() const { return series_; }
    [[nodiscard]] const MprView& view(MprOrientation o) const { return views_[static_cast<size_t>(o)]; }
    [[nodiscard]] Vec3 center() const { return center_; }
    void setCenter(const Vec3& p);
    void moveToSlice(MprOrientation o, int index);

    [[nodiscard]] SlabParams slab() const { return slab_; }
    void setSlab(const SlabParams& s);
    [[nodiscard]] Interpolation interpolation() const { return interp_; }
    void setInterpolation(Interpolation i);

    // Oblique MPR: rotates the plane of `o` around the crosshair, about the
    // normal of `around` (the view in which the user rotates the line).
    void rotate(MprOrientation o, MprOrientation around, double degrees);
    void resetOrientation();
    [[nodiscard]] bool isOblique() const { return oblique_; }
    [[nodiscard]] int version() const { return version_; }

Q_SIGNALS:
    void centerChanged();
    void renderingChanged();  // slab, interpolation or orientation changed

private:
    Vec3 clampToVolume(const Vec3& p) const;

    VolumePtr volume_;
    SeriesPtr series_;
    std::array<MprView, 3> views_;
    Vec3 center_;
    SlabParams slab_{0.0, SlabMode::MIP};
    Interpolation interp_ = Interpolation::Linear;
    bool oblique_ = false;
    int version_ = 0;
};

}  // namespace vtc
