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
    [[nodiscard]] const VolumePtr& volumePtr() const { return volume_; }
    [[nodiscard]] const SeriesPtr& series() const { return series_; }
    [[nodiscard]] const MprView& view(MprOrientation o) const { return views_[idx(o)]; }
    [[nodiscard]] Vec3 center() const { return center_; }
    void setCenter(const Vec3& p);
    void moveToSlice(MprOrientation o, int index);

    // Thick slab: each plane has its own thickness (dragged on its guide line
    // in the other views); the projection mode (MIP, MinIP, average) is shared.
    [[nodiscard]] SlabParams slab(MprOrientation o) const { return {thickness_[idx(o)], slabMode_}; }
    [[nodiscard]] SlabMode slabMode() const { return slabMode_; }
    void setSlab(const SlabParams& s);  // same thickness on the three planes
    void setSlabThickness(MprOrientation o, double mm);
    void setSlabMode(SlabMode mode);
    [[nodiscard]] Interpolation interpolation() const { return interp_; }
    void setInterpolation(Interpolation i);

    // Oblique MPR: rotates the plane of `o` around the crosshair, about the
    // normal of `around` (the view in which the user rotates the line).
    void rotate(MprOrientation o, MprOrientation around, double degrees);
    // Rotates the two other planes together (they stay perpendicular), as when
    // the user turns the crosshair in the view `around`.
    void rotateOthers(MprOrientation around, double degrees);
    void resetOrientation();
    [[nodiscard]] bool isOblique() const { return oblique_[0] || oblique_[1] || oblique_[2]; }
    [[nodiscard]] bool isOblique(MprOrientation o) const { return oblique_[idx(o)]; }
    // The straight (orthogonal) view of that orientation, before any rotation.
    [[nodiscard]] const MprView& orthogonalView(MprOrientation o) const { return ortho_[idx(o)]; }
    // Changes whenever the image of that plane must be recomputed.
    [[nodiscard]] int version(MprOrientation o) const { return versions_[idx(o)]; }

Q_SIGNALS:
    void centerChanged();
    void renderingChanged();  // slab, interpolation or orientation changed

private:
    static size_t idx(MprOrientation o) { return static_cast<size_t>(o); }
    Vec3 clampToVolume(const Vec3& p) const;
    void rotateView(MprOrientation o, const Vec3& axis, double radians);

    VolumePtr volume_;
    SeriesPtr series_;
    std::array<MprView, 3> views_;
    std::array<MprView, 3> ortho_;
    Vec3 center_;
    std::array<double, 3> thickness_{0.0, 0.0, 0.0};
    SlabMode slabMode_ = SlabMode::MIP;
    Interpolation interp_ = Interpolation::Linear;
    std::array<bool, 3> oblique_{false, false, false};
    std::array<int, 3> versions_{0, 0, 0};
};

}  // namespace vtc
