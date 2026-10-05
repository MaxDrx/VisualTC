#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <string>

#include "dicom/DicomTypes.h"
#include "imaging/PixelData.h"
#include "synchronization/SpatialSync.h"

namespace vtc {

// What a viewport displays: an indexed sequence of 2D images with known
// geometry. Implemented by native stacks (StackSource) and by MPR planes
// (MprSource), so every viewport tool works identically on both.
class ImageSource : public QObject {
    Q_OBJECT
public:
    enum class Kind { Stack, Mpr };

    using QObject::QObject;
    ~ImageSource() override = default;

    [[nodiscard]] virtual Kind kind() const = 0;
    [[nodiscard]] virtual int count() const = 0;
    // Decoded image if available; otherwise schedules loading and returns null.
    virtual DecodedFramePtr image(int index) = 0;
    [[nodiscard]] virtual QString errorAt(int index) const = 0;
    [[nodiscard]] virtual FrameGeometry geometryAt(int index) const = 0;
    [[nodiscard]] virtual QSize sizeAt(int index) const = 0;
    [[nodiscard]] virtual std::string frameOfReference() const = 0;
    // Representative header (overlays); may be null for synthetic images.
    [[nodiscard]] virtual const InstanceInfo* instanceAt(int index) const = 0;
    [[nodiscard]] virtual const FrameInfo* frameInfoAt(int /*index*/) const { return nullptr; }
    [[nodiscard]] virtual QString seriesLabel() const = 0;
    // Key under which the per-series view state is remembered (section 53).
    [[nodiscard]] virtual std::string stateKey() const = 0;
    // Key under which annotations of one image are stored.
    [[nodiscard]] virtual std::string annotationKey(int index) const = 0;
    [[nodiscard]] virtual bool isCt() const = 0;
    [[nodiscard]] virtual double frameRate() const { return 10.0; }
    virtual void prefetchAround(int /*index*/) {}
    virtual void stopPrefetch() {}
    // Initial image (e.g. middle slice for MPR).
    [[nodiscard]] virtual int initialIndex() const { return 0; }

    [[nodiscard]] PlaneRect planeAt(int index) const {
        PlaneRect r;
        r.geometry = geometryAt(index);
        const QSize s = sizeAt(index);
        r.width = s.width();
        r.height = s.height();
        r.frameOfReferenceUid = frameOfReference();
        return r;
    }
    [[nodiscard]] QString valueUnit() const { return isCt() ? QStringLiteral("HU") : QString(); }

Q_SIGNALS:
    void imageReady(int index);  // -1: any image may have changed
    void contentChanged();       // geometry/count changed (MPR)
};

}  // namespace vtc
