#pragma once

#include <memory>

#include "mpr/MprSession.h"
#include "viewer2d/ImageSource.h"
#include "viewer2d/Viewport.h"

namespace vtc {

// One MPR plane (axial, coronal or sagittal, possibly oblique) as an image
// source. Slice i is the plane through the crosshair moved to position i
// along this view's normal.
class MprSource : public ImageSource {
    Q_OBJECT
public:
    MprSource(std::shared_ptr<MprSession> session, MprOrientation orientation, QObject* parent = nullptr);

    [[nodiscard]] Kind kind() const override { return Kind::Mpr; }
    [[nodiscard]] int count() const override;
    DecodedFramePtr image(int index) override;
    [[nodiscard]] QString errorAt(int /*index*/) const override { return {}; }
    [[nodiscard]] FrameGeometry geometryAt(int index) const override;
    [[nodiscard]] QSize sizeAt(int index) const override;
    [[nodiscard]] std::string frameOfReference() const override;
    [[nodiscard]] const InstanceInfo* instanceAt(int index) const override;
    [[nodiscard]] const FrameInfo* frameInfoAt(int index) const override;
    [[nodiscard]] QString seriesLabel() const override;
    [[nodiscard]] std::string stateKey() const override;
    [[nodiscard]] std::string annotationKey(int index) const override;
    [[nodiscard]] bool isCt() const override;
    [[nodiscard]] int initialIndex() const override;

    [[nodiscard]] MprOrientation orientation() const { return orientation_; }
    [[nodiscard]] const std::shared_ptr<MprSession>& session() const { return session_; }
    [[nodiscard]] ReslicePlane planeAt(int index) const;
    // Crosshair lines of the two other planes, in this view's pixel coordinates.
    [[nodiscard]] std::vector<GuideLine> crosshairLines(int index) const;
    [[nodiscard]] Point2 crosshairPixel(int index) const;

    static QColor colorFor(MprOrientation o);

private:
    std::shared_ptr<MprSession> session_;
    MprOrientation orientation_;
    DecodedFramePtr cache_;
    int cacheIndex_ = -1;
    int cacheVersion_ = -1;
};

}  // namespace vtc
