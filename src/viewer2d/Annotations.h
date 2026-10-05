#pragma once

#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QTransform>
#include <memory>
#include <vector>

#include "dicom/DicomTypes.h"
#include "imaging/PixelData.h"
#include "measurements/MeasurementMath.h"

class QPainter;

namespace vtc {

enum class AnnotationKind { Distance, Angle, Cobb, Rectangle, Ellipse, Freehand };

// Everything a measurement needs to turn pixel coordinates into physical,
// clinically meaningful values.
struct MeasureContext {
    const DecodedFrame* frame = nullptr;
    double sx = 1.0;
    double sy = 1.0;
    SpacingSource spacing = SpacingSource::None;
    QString unit;  // "HU" for CT, empty for arbitrary units
    [[nodiscard]] bool calibrated() const { return spacing != SpacingSource::None; }
};

// A measurement drawn on one image. Points are continuous image pixel
// coordinates, so the measurement is independent of zoom, pan, rotation and
// flip, and is never burned into the pixels (section 12).
class Annotation {
public:
    explicit Annotation(AnnotationKind kind) : kind_(kind) {}

    [[nodiscard]] AnnotationKind kind() const { return kind_; }
    [[nodiscard]] int requiredPoints() const;  // 0 = variable (freehand)
    [[nodiscard]] bool isComplete() const;
    [[nodiscard]] bool isRoi() const {
        return kind_ == AnnotationKind::Rectangle || kind_ == AnnotationKind::Ellipse ||
               kind_ == AnnotationKind::Freehand;
    }

    std::vector<Point2> points;
    QPointF labelOffset{14.0, -14.0};  // screen pixels from the label anchor
    bool finished = false;

    [[nodiscard]] QStringList labelLines(const MeasureContext& ctx) const;
    // Modality values inside the ROI (for the histogram).
    [[nodiscard]] std::vector<double> roiValues(const MeasureContext& ctx) const;
    [[nodiscard]] Point2 labelAnchor() const;

    void paint(QPainter& p, const QTransform& toScreen, const MeasureContext& ctx, bool selected) const;
    [[nodiscard]] int hitPoint(const QPointF& pos, const QTransform& toScreen, double tol) const;
    [[nodiscard]] bool hitBody(const QPointF& pos, const QTransform& toScreen, double tol) const;
    [[nodiscard]] bool hitLabel(const QPointF& pos) const { return labelRect_.contains(pos); }

private:
    AnnotationKind kind_;
    mutable QRectF labelRect_;
};

using AnnotationPtr = std::shared_ptr<Annotation>;

QString formatLength(double mm, bool calibrated, SpacingSource source);

}  // namespace vtc
