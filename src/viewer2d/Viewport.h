#pragma once

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <optional>
#include <vector>

#include "imaging/WindowLevel.h"
#include "measurements/MeasurementMath.h"
#include "synchronization/SpatialSync.h"
#include "viewer2d/Annotations.h"
#include "viewer2d/ImageSource.h"

namespace vtc {

class AnnotationStore;

enum class Tool {
    WindowLevel,
    Pan,
    Zoom,
    Scroll,
    Distance,
    Angle,
    Cobb,
    RectRoi,
    EllipseRoi,
    FreehandRoi,
    Probe,
    Crosshair
};

// Line drawn over the image (reference lines, MPR crosshair), in image pixel
// coordinates of the currently displayed slice.
struct GuideLine {
    Point2 a;
    Point2 b;
    QColor color;
    bool dashed = false;
};

// Per-series display state, restored when the series is shown again in the
// same session (section 53).
struct ViewState {
    int slice = 0;
    double zoom = 1.0;  // logical screen pixels per mm
    QPointF pan;
    int rotation = 0;   // quarter turns clockwise
    double freeRotation = 0.0;
    bool flipH = false;
    bool flipV = false;
    bool invert = false;
    bool fit = true;
    bool hasWindow = false;
    double center = 0.0;
    double width = 1.0;
};

class Viewport : public QWidget {
    Q_OBJECT
public:
    explicit Viewport(AnnotationStore* store, QWidget* parent = nullptr);
    ~Viewport() override;

    void setSource(std::shared_ptr<ImageSource> source, const ViewState* restore = nullptr);
    [[nodiscard]] const std::shared_ptr<ImageSource>& source() const { return source_; }
    [[nodiscard]] ViewState viewState() const;
    void applyViewState(const ViewState& s);

    [[nodiscard]] int sliceIndex() const { return slice_; }
    void setSliceIndex(int index);
    void scrollBy(int delta);

    void setWindow(double center, double width);
    [[nodiscard]] double windowCenter() const { return center_; }
    [[nodiscard]] double windowWidth() const { return width_; }
    [[nodiscard]] bool hasWindow() const { return hasWindow_; }
    void resetWindowToDefault();
    void setVoiLut(bool on);
    [[nodiscard]] bool voiLutActive() const { return useVoiLut_; }

    void setInvert(bool on);
    [[nodiscard]] bool inverted() const { return invert_; }
    void rotate90(int quarterTurns);
    void setFreeRotation(double degrees);
    void flipHorizontal();
    void flipVertical();
    void fitToWindow();
    void actualSize();
    void resetView();
    void setZoom(double zoom, std::optional<QPointF> anchor = std::nullopt);
    [[nodiscard]] double zoom() const { return zoom_; }
    [[nodiscard]] QPointF pan() const { return pan_; }
    void setPan(QPointF pan);
    [[nodiscard]] double zoomPercent() const;

    void setSmooth(bool on);
    void setOverlaysVisible(bool on);
    [[nodiscard]] bool overlaysVisible() const { return overlays_; }
    void setActive(bool on);
    [[nodiscard]] bool isActive() const { return active_; }
    void setShowActiveFrame(bool on);
    // Text shown when the viewport is empty (e.g. how to open an exam).
    void setEmptyHint(const QString& text);
    void setTool(Tool tool);
    [[nodiscard]] Tool tool() const { return tool_; }

    void setReferenceLines(std::vector<GuideLine> lines);
    void setCrosshair(std::vector<GuideLine> lines, std::optional<Point2> center);

    // Cine
    void setCinePlaying(bool on);
    [[nodiscard]] bool cinePlaying() const { return cineTimer_.isActive(); }
    void setCineFps(double fps);
    [[nodiscard]] double cineFps() const { return cineFps_; }
    void setCineLoop(bool on) { cineLoop_ = on; }
    void setCineReverse(bool on) { cineDirection_ = on ? -1 : 1; }

    // Annotations
    void deleteSelectedAnnotation();
    void clearAnnotationsOnImage();
    [[nodiscard]] AnnotationPtr selectedAnnotation() const { return selected_; }
    [[nodiscard]] MeasureContext measureContext() const;
    void cancelPendingAnnotation();

    // Export
    [[nodiscard]] QImage renderImage(bool withOverlays, bool withAnnotations, bool hidePatientData = false) const;

    // Geometry
    [[nodiscard]] std::optional<PlaneRect> currentPlane() const;
    [[nodiscard]] QTransform imageToWidget() const;
    [[nodiscard]] std::optional<Vec3> patientPointAt(const QPointF& widgetPos) const;
    [[nodiscard]] DecodedFramePtr currentFrame() const { return shownFrame_; }
    // Reason why the current image could not be shown (empty when fine).
    [[nodiscard]] QString errorText() const { return error_; }

Q_SIGNALS:
    void activated(Viewport* vp);
    void sliceChanged(Viewport* vp);
    void viewTransformChanged(Viewport* vp);
    void windowChanged(Viewport* vp);
    void maximizeRequested(Viewport* vp);
    void cursorInfo(const QString& text);
    void crosshairDragged(Viewport* vp, const vtc::Vec3& point);
    void seriesDropped(Viewport* vp, const QString& seriesId);
    void annotationSelected(Viewport* vp);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class Drag {
        None,
        WindowLevel,
        Pan,
        Zoom,
        Scroll,
        NewAnnotation,
        Freehand,
        MovePoint,
        MoveAnnotation,
        MoveLabel,
        Probe,
        Crosshair
    };

    void onImageReady(int index);
    void onSourceContentChanged();
    void updateFrame();
    void ensureRendered() const;
    void paintContent(QPainter& p, const QSize& size, bool overlays, bool annotations, bool interactive) const;
    void drawOverlayText(QPainter& p, const QSize& size) const;
    void drawOrientationMarkers(QPainter& p, const QSize& size, const QTransform& t) const;
    void drawGuides(QPainter& p, const QTransform& t) const;
    void drawProbe(QPainter& p, const QTransform& t) const;
    [[nodiscard]] QTransform transformFor(const QSize& widgetSize) const;
    [[nodiscard]] double fitZoom(const QSize& widgetSize) const;
    [[nodiscard]] Point2 toImage(const QPointF& widgetPos) const;
    [[nodiscard]] std::string annotationKey() const;
    [[nodiscard]] Tool toolForButton(Qt::MouseButton b, Qt::KeyboardModifiers mods) const;
    void beginDrag(Tool tool, QMouseEvent* e);
    void startAnnotation(AnnotationKind kind, const Point2& p);
    void finishAnnotation();
    void updateCursorInfo(const QPointF& pos);
    void cineStep();

    AnnotationStore* store_;
    std::shared_ptr<ImageSource> source_;
    int slice_ = 0;
    DecodedFramePtr shownFrame_;
    int shownIndex_ = -1;
    bool loading_ = false;
    QString error_;

    // display state
    double center_ = 0.0;
    double width_ = 1.0;
    bool hasWindow_ = false;
    bool useVoiLut_ = false;
    bool invert_ = false;
    double zoom_ = 1.0;
    QPointF pan_;
    int rotation_ = 0;
    double freeRotation_ = 0.0;
    bool flipH_ = false;
    bool flipV_ = false;
    bool fit_ = true;
    bool smooth_ = true;
    bool overlays_ = true;
    bool active_ = false;
    bool showActiveFrame_ = false;
    Tool tool_ = Tool::WindowLevel;

    // render cache
    mutable DisplayRenderer renderer_;
    mutable QImage rendered_;
    // Owning reference (not a raw pointer): while it is held, no other frame
    // can be allocated at the same address and be mistaken for the image
    // that was rendered (stale pixels shown for another slice).
    mutable DecodedFramePtr renderedFrame_;
    mutable const InstanceInfo* renderedInstance_ = nullptr;
    mutable int renderedIndex_ = -1;
    mutable double renderedCenter_ = 0.0;
    mutable double renderedWidth_ = 0.0;
    mutable bool renderedInvert_ = false;
    mutable bool renderedVoiLut_ = false;
    mutable bool hidePatient_ = false;
    QString emptyHint_;

    std::vector<GuideLine> referenceLines_;
    std::vector<GuideLine> crosshairLines_;
    std::optional<Point2> crosshairCenter_;

    // interaction
    Drag drag_ = Drag::None;
    QPointF pressPos_;
    QPointF lastPos_;
    double scrollAccumulator_ = 0.0;
    double wheelAccumulator_ = 0.0;
    AnnotationPtr editing_;
    int editingPoint_ = -1;
    std::vector<Point2> editBefore_;
    QPointF labelBefore_;
    AnnotationPtr pending_;  // multi-step annotation (angle, cobb)
    AnnotationPtr selected_;
    std::optional<QPointF> probePos_;
    std::optional<QPointF> hoverPos_;

    // cine
    QTimer cineTimer_;
    double cineFps_ = 10.0;
    bool cineLoop_ = true;
    int cineDirection_ = 1;
};

}  // namespace vtc
