#include "viewer2d/Viewport.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLocale>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <cmath>
#include <numbers>

#include "app/AppSettings.h"
#include "app/Theme.h"
#include "dicom/TextUtil.h"
#include "imaging/Orientation.h"
#include "io/ThumbnailProvider.h"
#include "viewer2d/AnnotationStore.h"

namespace vtc {

namespace {

constexpr const char* kSeriesMime = "application/x-visualtc-series";

QString num(double v, int decimals) { return QLocale().toString(v, 'f', decimals); }

void drawOutlinedText(QPainter& p, const QPointF& pos, const QString& text, const QColor& color) {
    p.setPen(QColor(0, 0, 0, 220));
    for (const QPointF d : {QPointF(1, 1), QPointF(-1, 1), QPointF(1, -1), QPointF(-1, -1)}) {
        p.drawText(pos + d, text);
    }
    p.setPen(color);
    p.drawText(pos, text);
}

// Cursor shown over the round handles that turn the MPR planes.
QCursor makeRotateCursor() {
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(4.5, 4.5, 15, 15);
    for (const auto& [color, width] : {std::pair{QColor(0, 0, 0), 4.0}, std::pair{QColor(255, 255, 255), 2.0}}) {
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawArc(r, 100 * 16, 250 * 16);
        // Arrow head at the start of the arc (top), pointing clockwise.
        QPainterPath head;
        head.moveTo(9.0, 1.5);
        head.lineTo(13.5, 4.8);
        head.lineTo(9.0, 8.0);
        p.drawPath(head);
    }
    return QCursor(pm, 12, 12);
}

Qt::CursorShape cursorFor(Tool t) {
    switch (t) {
        case Tool::Pan: return Qt::OpenHandCursor;
        case Tool::Zoom: return Qt::SizeVerCursor;
        case Tool::Scroll: return Qt::SizeVerCursor;
        case Tool::WindowLevel: return Qt::ArrowCursor;
        default: return Qt::CrossCursor;
    }
}

}  // namespace

Viewport::Viewport(AnnotationStore* store, QWidget* parent) : QWidget(parent), store_(store) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(80, 80);
    smooth_ = AppSettings::instance().smoothInterpolation();
    overlays_ = AppSettings::instance().overlaysVisible();
    connect(&cineTimer_, &QTimer::timeout, this, &Viewport::cineStep);
    connect(store_, &AnnotationStore::changed, this, [this] { update(); });
    setCursor(cursorFor(tool_));
    rotateCursor_ = makeRotateCursor();
}

Viewport::~Viewport() {
    if (source_) {
        source_->stopPrefetch();
    }
}

// ------------------------------------------------------------- source/state

void Viewport::setSource(std::shared_ptr<ImageSource> source, const ViewState* restore) {
    if (source_) {
        disconnect(source_.get(), nullptr, this, nullptr);
        source_->stopPrefetch();
    }
    cineTimer_.stop();
    source_ = std::move(source);
    shownFrame_.reset();
    shownIndex_ = -1;
    error_.clear();
    pending_.reset();
    editing_.reset();
    setSelected(nullptr);
    referenceLines_.clear();
    mprGuides_.clear();
    mprCenter_.reset();
    hoverGuide_ = {};
    dragGuide_ = {};
    renderedFrame_.reset();
    if (!source_) {
        update();
        return;
    }
    connect(source_.get(), &ImageSource::imageReady, this, &Viewport::onImageReady);
    connect(source_.get(), &ImageSource::contentChanged, this, &Viewport::onSourceContentChanged);
    cineFps_ = source_->frameRate();
    if (restore != nullptr) {
        applyViewState(*restore);
    } else {
        slice_ = source_->initialIndex();
        zoom_ = 1.0;
        pan_ = {};
        rotation_ = 0;
        freeRotation_ = 0.0;
        flipH_ = flipV_ = false;
        invert_ = false;
        fit_ = true;
        hasWindow_ = false;
        useVoiLut_ = false;
        updateFrame();
    }
    Q_EMIT sliceChanged(this);
    update();
}

ViewState Viewport::viewState() const {
    ViewState s;
    s.slice = slice_;
    s.zoom = zoom_;
    s.pan = pan_;
    s.rotation = rotation_;
    s.freeRotation = freeRotation_;
    s.flipH = flipH_;
    s.flipV = flipV_;
    s.invert = invert_;
    s.colorMap = colorMap_;
    s.fit = fit_;
    s.hasWindow = hasWindow_;
    s.center = center_;
    s.width = width_;
    return s;
}

void Viewport::applyViewState(const ViewState& s) {
    slice_ = s.slice;
    zoom_ = s.zoom;
    pan_ = s.pan;
    rotation_ = s.rotation;
    freeRotation_ = s.freeRotation;
    flipH_ = s.flipH;
    flipV_ = s.flipV;
    invert_ = s.invert;
    colorMap_ = s.colorMap;
    fit_ = s.fit;
    hasWindow_ = s.hasWindow;
    center_ = s.center;
    width_ = s.width;
    updateFrame();
    update();
}

void Viewport::onImageReady(int index) {
    if (index == slice_ || index == -1 || !shownFrame_) {
        updateFrame();
        update();
    }
}

void Viewport::onSourceContentChanged() {
    renderedFrame_.reset();
    updateFrame();
    update();
}

void Viewport::updateFrame() {
    if (!source_ || source_->count() == 0) {
        shownFrame_.reset();
        return;
    }
    slice_ = std::clamp(slice_, 0, source_->count() - 1);
    const DecodedFramePtr f = source_->image(slice_);
    if (f) {
        shownFrame_ = f;
        shownIndex_ = slice_;
        loading_ = false;
        error_.clear();
        if (!hasWindow_) {
            const FrameInfo* info = source_->frameInfoAt(slice_);
            const WindowPreset w = info != nullptr ? defaultWindowFor(*info, *f) : autoWindow(*f);
            center_ = w.center;
            width_ = std::max(1.0, w.width);
            hasWindow_ = true;
            Q_EMIT windowChanged(this);
        }
    } else {
        error_ = source_->errorAt(slice_);
        loading_ = error_.isEmpty();
        if (!error_.isEmpty()) {
            shownFrame_.reset();  // never show another slice's pixels as if they were this one
            shownIndex_ = slice_;
        }
    }
    source_->prefetchAround(slice_);
}

void Viewport::setSliceIndex(int index) {
    if (!source_ || source_->count() == 0) {
        return;
    }
    index = std::clamp(index, 0, source_->count() - 1);
    if (index == slice_ && shownIndex_ == slice_) {
        return;
    }
    slice_ = index;
    if (pending_) {
        pending_.reset();
    }
    setSelected(nullptr);
    updateFrame();
    update();
    Q_EMIT sliceChanged(this);
}

void Viewport::scrollBy(int delta) {
    if (!source_ || delta == 0) {
        return;
    }
    setSliceIndex(slice_ + delta);
}

// -------------------------------------------------------------- display ops

void Viewport::setWindow(double center, double width) {
    center_ = center;
    width_ = std::max(1.0, width);
    hasWindow_ = true;
    useVoiLut_ = false;
    update();
    Q_EMIT windowChanged(this);
}

void Viewport::resetWindowToDefault() {
    hasWindow_ = false;
    useVoiLut_ = false;
    updateFrame();
    update();
    Q_EMIT windowChanged(this);
}

void Viewport::setVoiLut(bool on) {
    const InstanceInfo* inst = source_ ? source_->instanceAt(slice_) : nullptr;
    useVoiLut_ = on && inst != nullptr && inst->voiLut.has_value();
    update();
    Q_EMIT windowChanged(this);
}

void Viewport::setInvert(bool on) {
    invert_ = on;
    update();
    Q_EMIT windowChanged(this);
}

void Viewport::setColorMap(ColorMap map) {
    if (colorMap_ == map) {
        return;
    }
    colorMap_ = map;
    update();
    Q_EMIT windowChanged(this);
}

void Viewport::rotate90(int quarterTurns) {
    rotation_ = ((rotation_ + quarterTurns) % 4 + 4) % 4;
    update();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::setFreeRotation(double degrees) {
    freeRotation_ = std::fmod(degrees, 360.0);
    update();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::flipHorizontal() {
    flipH_ = !flipH_;
    update();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::flipVertical() {
    flipV_ = !flipV_;
    update();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::fitToWindow() {
    fit_ = true;
    pan_ = {};
    update();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::actualSize() {
    if (!shownFrame_ || !source_) {
        return;
    }
    const auto g = source_->geometryAt(shownIndex_);
    const double sx = g.hasSpacing() ? g.spacingX : 1.0;
    fit_ = false;
    zoom_ = 1.0 / (sx * devicePixelRatioF());
    pan_ = {};
    update();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::resetView() {
    rotation_ = 0;
    freeRotation_ = 0.0;
    flipH_ = flipV_ = false;
    invert_ = false;
    fit_ = true;
    pan_ = {};
    resetWindowToDefault();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::setZoom(double zoom, std::optional<QPointF> anchor) {
    if (!shownFrame_) {
        return;
    }
    const QPointF a = anchor.value_or(QPointF(width() / 2.0, height() / 2.0));
    const Point2 before = toImage(a);
    if (fit_) {
        zoom_ = fitZoom(size());
        fit_ = false;
    }
    zoom_ = std::clamp(zoom, 0.01, 400.0);
    const QPointF after = transformFor(size()).map(QPointF(before.x, before.y));
    pan_ += a - after;
    update();
    Q_EMIT viewTransformChanged(this);
}

void Viewport::setPan(QPointF pan) {
    pan_ = pan;
    update();
}

double Viewport::zoomPercent() const {
    if (!shownFrame_ || !source_) {
        return 100.0;
    }
    const auto g = source_->geometryAt(shownIndex_);
    const double sx = g.hasSpacing() ? g.spacingX : 1.0;
    const double z = fit_ ? fitZoom(size()) : zoom_;
    return z * sx * devicePixelRatioF() * 100.0;
}

void Viewport::setSmooth(bool on) {
    smooth_ = on;
    update();
}

void Viewport::setOverlaysVisible(bool on) {
    overlays_ = on;
    update();
}

void Viewport::setActive(bool on) {
    if (active_ != on) {
        active_ = on;
        update();
    }
}

void Viewport::setEmptyHint(const QString& text) {
    emptyHint_ = text;
    update();
}

void Viewport::setShowActiveFrame(bool on) {
    showActiveFrame_ = on;
    update();
}

void Viewport::setTool(Tool tool) {
    cancelPendingAnnotation();
    tool_ = tool;
    hoverGuide_ = {};
    setCursor(cursorFor(tool));
}

void Viewport::setReferenceLines(std::vector<GuideLine> lines) {
    referenceLines_ = std::move(lines);
    update();
}

void Viewport::setMprGuides(std::vector<MprGuide> guides, std::optional<Point2> center) {
    mprGuides_ = std::move(guides);
    mprCenter_ = center;
    if (!mprCenter_ || mprGuides_.empty()) {
        hoverGuide_ = {};
    }
    update();
}

std::optional<Viewport::GuideHandles> Viewport::guideHandles(size_t guide) const {
    return guideHandlesFor(guide, transformFor(size()), size());
}

std::optional<Viewport::GuideHandles> Viewport::guideHandlesFor(size_t guide, const QTransform& t,
                                                                const QSize& size) const {
    if (!mprCenter_ || guide >= mprGuides_.size() || !shownFrame_) {
        return std::nullopt;
    }
    const MprGuide& g = mprGuides_[guide];
    const QPointF c0(mprCenter_->x, mprCenter_->y);
    GuideHandles h;
    h.center = t.map(c0);
    QPointF d = t.map(c0 + QPointF(g.direction.x, g.direction.y)) - h.center;
    const double dl = std::hypot(d.x(), d.y());
    if (!(dl > 1e-9)) {
        return std::nullopt;
    }
    d /= dl;
    QPointF m = t.map(c0 + QPointF(g.mmOffset.x, g.mmOffset.y)) - h.center;
    m -= d * QPointF::dotProduct(m, d);  // keep only the part across the line
    const double ml = std::hypot(m.x(), m.y());
    if (!(ml > 1e-9)) {
        return std::nullopt;
    }
    h.dir = d;
    h.normal = m / ml;
    h.pxPerMm = ml;
    const double side = std::min(size.width(), size.height());
    const double rRotate = std::max(40.0, 0.38 * side);
    const double rSlab = std::max(24.0, 0.2 * side);
    const double off = std::max(g.thickness / 2.0 * h.pxPerMm, 9.0);
    h.rotate[0] = h.center + d * rRotate;
    h.rotate[1] = h.center - d * rRotate;
    h.slab[0] = h.center + d * rSlab + h.normal * off;
    h.slab[1] = h.center - d * rSlab - h.normal * off;
    return h;
}

// --------------------------------------------------------------------- cine

void Viewport::setCinePlaying(bool on) {
    if (on && source_ && source_->count() > 1) {
        cineTimer_.start(static_cast<int>(std::lround(1000.0 / std::clamp(cineFps_, 1.0, 120.0))));
    } else {
        cineTimer_.stop();
    }
    update();
}

void Viewport::setCineFps(double fps) {
    cineFps_ = std::clamp(fps, 1.0, 120.0);
    if (cineTimer_.isActive()) {
        cineTimer_.start(static_cast<int>(std::lround(1000.0 / cineFps_)));
    }
}

void Viewport::cineStep() {
    if (!source_) {
        cineTimer_.stop();
        return;
    }
    // Do not advance while the current frame is still loading: playback
    // speed then degrades gracefully instead of showing blank frames.
    if (!shownFrame_ || shownIndex_ != slice_) {
        return;
    }
    int next = slice_ + cineDirection_;
    if (next >= source_->count() || next < 0) {
        if (!cineLoop_) {
            cineTimer_.stop();
            update();
            return;
        }
        next = cineDirection_ > 0 ? 0 : source_->count() - 1;
    }
    setSliceIndex(next);
}

// -------------------------------------------------------------- annotations

std::string Viewport::annotationKey() const { return source_ ? source_->annotationKey(slice_) : std::string(); }

MeasureContext Viewport::measureContext() const {
    MeasureContext ctx;
    if (!source_ || !shownFrame_) {
        return ctx;
    }
    ctx.frame = shownFrame_.get();
    const auto g = source_->geometryAt(shownIndex_);
    ctx.spacing = g.spacingSource;
    ctx.sx = g.spacingX;
    ctx.sy = g.spacingY;
    ctx.unit = source_->valueUnit();
    return ctx;
}

void Viewport::cancelPendingAnnotation() {
    if (pending_ || editing_) {
        pending_.reset();
        if (drag_ == Drag::NewAnnotation || drag_ == Drag::Freehand) {
            drag_ = Drag::None;
        }
        editing_.reset();
        update();
    }
}

void Viewport::deleteSelectedAnnotation() {
    if (selected_) {
        store_->remove(annotationKey(), selected_);
        setSelected(nullptr);
        update();
    }
}

void Viewport::clearAnnotationsOnImage() {
    store_->clearImage(annotationKey());
    setSelected(nullptr);
    update();
}

void Viewport::startAnnotation(AnnotationKind kind, const Point2& p) {
    editing_ = std::make_shared<Annotation>(kind);
    editing_->points = {p, p};
    if (kind == AnnotationKind::Freehand) {
        editing_->points = {p};
    }
    editingPoint_ = 1;
    setSelected(nullptr);
}

void Viewport::finishAnnotation() {
    AnnotationPtr a = editing_ ? editing_ : pending_;
    if (!a) {
        return;
    }
    a->finished = true;
    editing_.reset();
    pending_.reset();
    setSelected(a);  // before the store notifies: the new ROI is already the selection
    store_->add(annotationKey(), a);
}

void Viewport::setSelected(const AnnotationPtr& a) {
    if (selected_ == a) {
        return;
    }
    selected_ = a;
    Q_EMIT annotationSelected(this);
}

// ---------------------------------------------------------------- geometry

double Viewport::fitZoom(const QSize& widgetSize) const {
    if (!shownFrame_ || !source_) {
        return 1.0;
    }
    const auto g = source_->geometryAt(shownIndex_);
    const double sx = g.hasSpacing() ? g.spacingX : 1.0;
    const double sy = g.hasSpacing() ? g.spacingY : 1.0;
    double w = shownFrame_->width * sx;
    double h = shownFrame_->height * sy;
    if (const auto e = source_->fitExtentMm(); e && e->width() > 0.0 && e->height() > 0.0) {
        w = e->width();
        h = e->height();
    }
    const double a = (rotation_ * 90.0 + freeRotation_) * std::numbers::pi / 180.0;
    const double wr = std::abs(w * std::cos(a)) + std::abs(h * std::sin(a));
    const double hr = std::abs(w * std::sin(a)) + std::abs(h * std::cos(a));
    if (wr <= 0.0 || hr <= 0.0) {
        return 1.0;
    }
    return 0.98 * std::min(widgetSize.width() / wr, widgetSize.height() / hr);
}

QTransform Viewport::transformFor(const QSize& widgetSize) const {
    QTransform t;
    if (!shownFrame_ || !source_) {
        return t;
    }
    const auto g = source_->geometryAt(shownIndex_);
    const double sx = g.hasSpacing() ? g.spacingX : 1.0;
    const double sy = g.hasSpacing() ? g.spacingY : 1.0;
    const double z = fit_ ? fitZoom(widgetSize) : zoom_;
    // Composition (applied to a point from last to first):
    // image px -> centred -> mm -> zoom -> rotation -> screen-space flip -> pan
    t.translate(widgetSize.width() / 2.0 + pan_.x(), widgetSize.height() / 2.0 + pan_.y());
    t.scale(flipH_ ? -1.0 : 1.0, flipV_ ? -1.0 : 1.0);
    t.rotate(rotation_ * 90.0 + freeRotation_);
    t.scale(z * sx, z * sy);
    t.translate(-(shownFrame_->width - 1) / 2.0, -(shownFrame_->height - 1) / 2.0);
    return t;
}

QTransform Viewport::imageToWidget() const { return transformFor(size()); }

Point2 Viewport::toImage(const QPointF& widgetPos) const {
    bool invertible = false;
    const QTransform inv = transformFor(size()).inverted(&invertible);
    if (!invertible) {
        return {};
    }
    const QPointF p = inv.map(widgetPos);
    return {p.x(), p.y()};
}

std::optional<PlaneRect> Viewport::currentPlane() const {
    if (!source_ || source_->count() == 0) {
        return std::nullopt;
    }
    PlaneRect r = source_->planeAt(slice_);
    if (!r.geometry.isSpatial()) {
        return std::nullopt;
    }
    return r;
}

std::optional<Vec3> Viewport::patientPointAt(const QPointF& widgetPos) const {
    if (!source_ || !shownFrame_) {
        return std::nullopt;
    }
    const auto g = source_->geometryAt(slice_);
    if (!g.isSpatial()) {
        return std::nullopt;
    }
    const Point2 ip = toImage(widgetPos);
    return g.pixelToPatient(ip.x, ip.y);
}

// ---------------------------------------------------------------- painting

void Viewport::ensureRendered() const {
    if (!shownFrame_) {
        return;
    }
    const InstanceInfo* instance = source_ ? source_->instanceAt(shownIndex_) : nullptr;
    if (renderedFrame_ == shownFrame_ && renderedInstance_ == instance && renderedIndex_ == shownIndex_ &&
        renderedCenter_ == center_ && renderedWidth_ == width_ && renderedInvert_ == invert_ &&
        renderedVoiLut_ == useVoiLut_ && renderedColorMap_ == colorMap_ && !rendered_.isNull()) {
        return;
    }
    const DecodedFrame& f = *shownFrame_;
    DisplayParams params;
    params.center = center_;
    params.width = width_;
    params.invert = invert_;
    const InstanceInfo* inst = instance;
    if (inst != nullptr) {
        params.function = parseVoiFunction(inst->voiLutFunction);
        if (useVoiLut_ && inst->voiLut) {
            params.voiLut = &*inst->voiLut;
        }
    }
    if (f.isColor()) {
        if (rendered_.size() != QSize(f.width, f.height) || rendered_.format() != QImage::Format_RGB32) {
            rendered_ = QImage(f.width, f.height, QImage::Format_RGB32);
        }
        renderer_.renderRgb32(f, params, reinterpret_cast<std::uint32_t*>(rendered_.bits()),
                              static_cast<int>(rendered_.bytesPerLine()));
    } else if (colorMap_ == ColorMap::Gray) {
        if (rendered_.size() != QSize(f.width, f.height) || rendered_.format() != QImage::Format_Grayscale8) {
            rendered_ = QImage(f.width, f.height, QImage::Format_Grayscale8);
        }
        renderer_.renderGray(f, params, rendered_.bits(), static_cast<int>(rendered_.bytesPerLine()));
    } else {
        // Same grey levels, shown through the colour table (one byte per pixel).
        if (rendered_.size() != QSize(f.width, f.height) || rendered_.format() != QImage::Format_Indexed8) {
            rendered_ = QImage(f.width, f.height, QImage::Format_Indexed8);
        }
        if (renderedColorMap_ != colorMap_ || rendered_.colorCount() != 256) {
            const auto& table = colorMapTable(colorMap_);
            rendered_.setColorTable(QList<QRgb>(table.begin(), table.end()));
        }
        renderer_.renderGray(f, params, rendered_.bits(), static_cast<int>(rendered_.bytesPerLine()));
    }
    renderedFrame_ = shownFrame_;
    renderedInstance_ = instance;
    renderedIndex_ = shownIndex_;
    renderedCenter_ = center_;
    renderedWidth_ = width_;
    renderedInvert_ = invert_;
    renderedVoiLut_ = useVoiLut_;
    renderedColorMap_ = colorMap_;
}

void Viewport::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    paintContent(p, size(), overlays_, true, true);
}

void Viewport::paintContent(QPainter& p, const QSize& size, bool overlays, bool annotations, bool interactive) const {
    p.fillRect(QRect(QPoint(0, 0), size), Qt::black);
    QFont baseFont = font();
    baseFont.setPixelSize(12);
    p.setFont(baseFont);

    if (!source_) {
        if (interactive) {
            p.setPen(QColor(0x6A, 0x6A, 0x6A));
            p.drawText(QRect(QPoint(0, 0), size).adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap,
                       emptyHint_.isEmpty() ? tr("Arraste uma série para cá\nou dê um duplo clique em uma série")
                                            : emptyHint_);
        }
        if (interactive && active_ && showActiveFrame_) {
            p.setPen(QPen(Theme::colors().accent, 1));
            p.drawRect(QRect(QPoint(0, 0), size).adjusted(0, 0, -1, -1));
        }
        return;
    }

    const QTransform t = transformFor(size);
    if (shownFrame_) {
        ensureRendered();
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, smooth_);
        p.setTransform(t);
        p.drawImage(QRectF(-0.5, -0.5, shownFrame_->width, shownFrame_->height), rendered_);
        p.restore();
        drawGuides(p, t, size, interactive);
        if (annotations) {
            const MeasureContext ctx = measureContext();
            for (const auto& a : store_->list(annotationKey())) {
                a->paint(p, t, ctx, interactive && a == selected_);
            }
            if (editing_) {
                editing_->paint(p, t, ctx, true);
            }
            if (pending_ && pending_ != editing_) {
                if (interactive && hoverPos_ && pending_->kind() == AnnotationKind::Angle &&
                    pending_->points.size() == 2) {
                    // Live preview of the second arm while waiting for the click.
                    Annotation preview = *pending_;
                    preview.points.push_back(toImage(*hoverPos_));
                    preview.paint(p, t, ctx, true);
                } else {
                    pending_->paint(p, t, ctx, true);
                }
            }
        }
        if (interactive) {
            drawProbe(p, t);
        }
    }

    if (overlays) {
        drawOverlayText(p, size);
        if (shownFrame_) {
            drawOrientationMarkers(p, size, t);
        }
    }

    if (!error_.isEmpty()) {
        QFont f = baseFont;
        f.setPixelSize(13);
        p.setFont(f);
        const QRect box(QPoint(20, size.height() / 2 - 40), QSize(size.width() - 40, 80));
        p.setPen(Theme::colors().warning);
        p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, tr("Imagem indisponível: %1").arg(error_));
        p.setFont(baseFont);
    } else if (loading_ && interactive) {
        const QString msg = tr("Carregando…");
        const QFontMetricsF fm(p.font());
        drawOutlinedText(p, QPointF((size.width() - fm.horizontalAdvance(msg)) / 2.0, size.height() - 30.0), msg,
                         Theme::colors().accent);
    }

    if (interactive && cineTimer_.isActive()) {
        drawOutlinedText(p, QPointF(size.width() / 2.0 - 30, 18), tr("▶ Cine %1 fps").arg(num(cineFps_, 0)),
                         Theme::colors().accent);
    }

    if (interactive && active_ && showActiveFrame_) {
        p.setPen(QPen(Theme::colors().accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(QPoint(0, 0), size).adjusted(0, 0, -1, -1));
    }
}

void Viewport::drawGuides(QPainter& p, const QTransform& t, const QSize& size, bool interactive) const {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto& g : referenceLines_) {
        QPen pen(g.color, 1.6, g.dashed ? Qt::DashLine : Qt::SolidLine);  // visible on Retina/4K screens
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawLine(t.map(QPointF(g.a.x, g.a.y)), t.map(QPointF(g.b.x, g.b.y)));
    }
    p.restore();
    drawMprGuides(p, t, size, interactive);
}

void Viewport::drawMprGuides(QPainter& p, const QTransform& t, const QSize& size, bool interactive) const {
    if (!mprCenter_) {
        return;
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c = t.map(QPointF(mprCenter_->x, mprCenter_->y));
    // Long enough to cross the whole widget from wherever the centre is.
    const double reach = std::hypot(size.width(), size.height()) + std::hypot(c.x(), c.y()) +
                         std::hypot(size.width() - c.x(), size.height() - c.y());
    const GuideHit& focus = drag_ != Drag::None ? dragGuide_ : hoverGuide_;
    for (size_t i = 0; i < mprGuides_.size(); ++i) {
        const auto h = guideHandlesFor(i, t, size);
        if (!h) {
            continue;
        }
        const MprGuide& g = mprGuides_[i];
        const bool hot = interactive && focus.guide == static_cast<int>(i) && focus.part != GuideHit::Center;
        // A small gap keeps the anatomy at the crosshair visible.
        const double gap = interactive ? 12.0 : 0.0;
        QPen pen(g.color, hot ? 2.4 : 1.3);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawLine(c + h->dir * gap, c + h->dir * reach);
        p.drawLine(c - h->dir * gap, c - h->dir * reach);
        if (g.thickness > 0.0) {  // slab boundaries
            QColor sc = g.color;
            sc.setAlpha(200);
            QPen dash(sc, hot ? 1.6 : 1.0, Qt::DashLine);
            dash.setCosmetic(true);
            p.setPen(dash);
            const QPointF o = h->normal * (g.thickness / 2.0 * h->pxPerMm);
            for (const QPointF& off : {o, -o}) {
                p.drawLine(c + off - h->dir * reach, c + off + h->dir * reach);
            }
        }
        if (!interactive) {
            continue;
        }
        QPen outline(QColor(0, 0, 0, 210), 1.0);
        outline.setCosmetic(true);
        // Round handles at both ends: turn the planes (oblique MPR).
        for (int k = 0; k < 2; ++k) {
            const bool on = hot && focus.part == GuideHit::Rotate && focus.side == (k == 0 ? 1 : -1);
            p.setPen(outline);
            p.setBrush(g.color);
            const double r = on ? 6.5 : 5.0;
            p.drawEllipse(h->rotate[k], r, r);
        }
        // Bars beside the line: slab thickness.
        for (int k = 0; k < 2; ++k) {
            const bool on = hot && focus.part == GuideHit::Slab && focus.side == (k == 0 ? 1 : -1);
            const double len = on ? 9.0 : 7.0;
            const double wid = on ? 3.2 : 2.4;
            const QPointF a = h->slab[k];
            const QPolygonF bar({a - h->dir * len - h->normal * wid, a + h->dir * len - h->normal * wid,
                                 a + h->dir * len + h->normal * wid, a - h->dir * len + h->normal * wid});
            p.setPen(outline);
            p.setBrush(g.color);
            p.drawPolygon(bar);
        }
        // Hint next to the handle under the mouse.
        QString hint;
        QPointF at;
        if (hot && focus.part == GuideHit::Slab) {
            hint = g.thickness > 0.0 ? QStringLiteral("%1 %2 mm").arg(g.slabMode, num(g.thickness, 1))
                                     : tr("Arraste para dar espessura (MIP/MinIP)");
            at = h->slab[focus.side > 0 ? 0 : 1];
        } else if (hot && focus.part == GuideHit::Rotate) {
            hint = tr("Arraste para girar (MPR oblíquo)");
            at = h->rotate[focus.side < 0 ? 1 : 0];
        }
        if (!hint.isEmpty()) {
            QFont f = p.font();
            f.setPixelSize(12);
            p.setFont(f);
            const QFontMetricsF fm(f);
            const double w = fm.horizontalAdvance(hint);
            QPointF pos = at + QPointF(12, -10);
            pos.setX(std::clamp(pos.x(), 4.0, std::max(4.0, size.width() - w - 4.0)));
            pos.setY(std::clamp(pos.y(), fm.ascent() + 4.0, std::max(fm.ascent() + 4.0, size.height() - 6.0)));
            drawOutlinedText(p, pos, hint, g.color.lighter(130));
        }
    }
    // Crosshair centre (drag it to move both lines).
    const bool centerHot = interactive && focus.part == GuideHit::Center;
    QPen pen(QColor(255, 255, 255, 220), centerHot ? 1.8 : 1.1);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(centerHot ? QColor(255, 255, 255, 60) : QColor(Qt::transparent));
    p.drawEllipse(c, centerHot ? 6.5 : 5.0, centerHot ? 6.5 : 5.0);
    p.restore();
}

void Viewport::drawProbe(QPainter& p, const QTransform& t) const {
    if (!probePos_ || !shownFrame_) {
        return;
    }
    const Point2 ip = toImage(*probePos_);
    const int x = static_cast<int>(std::lround(ip.x));
    const int y = static_cast<int>(std::lround(ip.y));
    QStringList lines;
    lines << tr("Pixel: (%1, %2)").arg(x).arg(y);
    if (x >= 0 && y >= 0 && x < shownFrame_->width && y < shownFrame_->height) {
        if (shownFrame_->isColor()) {
            const std::size_t i = (static_cast<std::size_t>(y) * shownFrame_->width + x) * 3;
            lines << tr("RGB: %1, %2, %3").arg(shownFrame_->data[i]).arg(shownFrame_->data[i + 1]).arg(shownFrame_->data[i + 2]);
        } else {
            const double raw = shownFrame_->rawAt(x, y);
            const double v = shownFrame_->valueAt(x, y);
            if (std::isnan(v)) {
                lines << tr("Fora do volume");
            } else {
                const QString unit = source_->valueUnit();
                lines << tr("Valor: %1%2").arg(num(v, 1), unit.isEmpty() ? QString() : " " + unit);
                if (source_->kind() == ImageSource::Kind::Stack) {
                    lines << tr("Valor armazenado: %1").arg(num(raw, 0));
                }
            }
        }
    }
    const auto g = source_->geometryAt(shownIndex_);
    if (g.isSpatial()) {
        const Vec3 pp = g.pixelToPatient(ip.x, ip.y);
        lines << tr("Paciente (mm): x %1  y %2  z %3").arg(num(pp.x, 1), num(pp.y, 1), num(pp.z, 1));
    }
    const QPointF c = *probePos_;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(Theme::measurementSelected(), 1);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.drawLine(c + QPointF(-8, 0), c + QPointF(-3, 0));
    p.drawLine(c + QPointF(3, 0), c + QPointF(8, 0));
    p.drawLine(c + QPointF(0, -8), c + QPointF(0, -3));
    p.drawLine(c + QPointF(0, 3), c + QPointF(0, 8));
    const QFontMetricsF fm(p.font());
    double w = 0;
    for (const auto& l : lines) {
        w = std::max(w, fm.horizontalAdvance(l));
    }
    QRectF box(c + QPointF(14, 10), QSizeF(w + 12, fm.height() * lines.size() + 8));
    if (box.right() > width()) {
        box.moveRight(c.x() - 14);
    }
    if (box.bottom() > height()) {
        box.moveBottom(c.y() - 10);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 190));
    p.drawRoundedRect(box, 4, 4);
    p.setPen(Theme::measurementSelected());
    for (int i = 0; i < lines.size(); ++i) {
        p.drawText(QPointF(box.left() + 6, box.top() + 4 + fm.ascent() + i * fm.height()), lines[i]);
    }
    p.restore();
    (void)t;
}

void Viewport::drawOverlayText(QPainter& p, const QSize& size) const {
    const InstanceInfo* inst = source_->instanceAt(slice_);
    const QFontMetricsF fm(p.font());
    const double lh = fm.height();
    const double margin = 8.0;
    const QColor color = Theme::overlayText();
    const QColor dim(0xB4, 0xB4, 0xB4);

    QStringList tl;
    QStringList tr_;
    QStringList bl;
    QStringList br;
    QStringList flags;
    if (inst != nullptr) {
        if (hidePatient_) {
            // Exported without identification: name, ID, dates and
            // institution are all omitted.
            tl << tr("(identificação oculta)");
        } else {
            tl << QString::fromStdString(inst->patientName.empty() ? std::string("(sem nome)") : inst->patientName);
            tl << tr("ID: %1").arg(QString::fromStdString(inst->patientId.empty() ? "N/A" : inst->patientId));
            QString dt = QString::fromStdString(formatDicomDate(inst->studyDate));
            if (!inst->studyTime.empty()) {
                dt += " " + QString::fromStdString(formatDicomTime(inst->studyTime));
            }
            tl << (dt.trimmed().isEmpty() ? tr("Data: N/A") : dt);
            if (!inst->institutionName.empty()) {
                tr_ << QString::fromStdString(inst->institutionName);
            }
        }
        if (!inst->studyDescription.empty()) {
            tr_ << QString::fromStdString(inst->studyDescription);
        }
        if (inst->lossyCompressed) {
            flags << tr("COMPRESSÃO COM PERDAS");
        }
    }
    tr_ << source_->seriesLabel();
    if (inst != nullptr && !inst->modality.empty()) {
        tr_ << QString::fromStdString(inst->modality);
    }

    if (useVoiLut_) {
        bl << tr("VOI LUT (DICOM)");
    } else {
        bl << tr("WW %1  WL %2").arg(num(width_, 0), num(center_, 0));
    }
    if (colorMap_ != ColorMap::Gray && shownFrame_ && !shownFrame_->isColor()) {
        bl << tr("LUT %1").arg(QString::fromStdString(colorMapName(colorMap_)));
    }
    bl << tr("Zoom %1%").arg(num(zoomPercent(), 0));
    const auto g = source_->geometryAt(slice_);
    if (source_->kind() == ImageSource::Kind::Mpr) {
        bl << (g.sliceThickness ? tr("Slab %1 mm").arg(num(*g.sliceThickness, 1)) : tr("MPR plano fino"));
    } else if (g.sliceThickness) {
        bl << tr("Espessura %1 mm").arg(num(*g.sliceThickness, 2));
    } else {
        bl << tr("Espessura N/A");
    }

    br << tr("Imagem %1 / %2").arg(slice_ + 1).arg(source_->count());
    if (g.hasPosition && g.hasOrientation) {
        br << tr("Posição %1 mm").arg(num(g.position.dot(g.normal()), 2));
    }
    if (shownFrame_) {
        br << QStringLiteral("%1 × %2").arg(shownFrame_->width).arg(shownFrame_->height);
    }

    if (invert_) {
        flags << tr("INVERTIDO");
    }
    if (flipH_) {
        flags << tr("ESPELHADO H");
    }
    if (flipV_) {
        flags << tr("ESPELHADO V");
    }
    if (rotation_ != 0 || std::abs(freeRotation_) > 1e-6) {
        flags << tr("ROTAÇÃO %1°").arg(num(std::fmod(rotation_ * 90.0 + freeRotation_ + 360.0, 360.0), 0));
    }
    if (!g.hasSpacing()) {
        flags << tr("SEM CALIBRAÇÃO (medidas em pixels)");
    } else if (g.spacingSource == SpacingSource::ImagerPixelSpacing) {
        flags << tr("CALIBRAÇÃO NO DETECTOR");
    }

    // Corner texts never reach the centre of the edge, where the orientation
    // markers are drawn.
    const int maxW = std::max(40, static_cast<int>(size.width() * 0.42));
    for (auto* list : {&tl, &tr_, &bl, &br}) {
        for (auto& line : *list) {
            line = fm.elidedText(line, Qt::ElideRight, maxW);
        }
    }
    for (int i = 0; i < tl.size(); ++i) {
        drawOutlinedText(p, QPointF(margin, margin + fm.ascent() + i * lh), tl[i], i == 0 ? color : dim);
    }
    for (int i = 0; i < tr_.size(); ++i) {
        const double w = fm.horizontalAdvance(tr_[i]);
        drawOutlinedText(p, QPointF(size.width() - margin - w, margin + fm.ascent() + i * lh), tr_[i], dim);
    }
    const double bottom = size.height() - margin - fm.descent();
    for (int i = 0; i < bl.size(); ++i) {
        drawOutlinedText(p, QPointF(margin, bottom - (bl.size() - 1 - i) * lh), bl[i], color);
    }
    for (int i = 0; i < flags.size(); ++i) {
        drawOutlinedText(p, QPointF(margin, bottom - (bl.size() + flags.size() - 1 - i) * lh - 4), flags[i],
                         Theme::colors().warning);
    }
    for (int i = 0; i < br.size(); ++i) {
        const double w = fm.horizontalAdvance(br[i]);
        drawOutlinedText(p, QPointF(size.width() - margin - w, bottom - (br.size() - 1 - i) * lh), br[i], color);
    }
}

void Viewport::drawOrientationMarkers(QPainter& p, const QSize& size, const QTransform& t) const {
    const auto g = source_->geometryAt(shownIndex_);
    if (!g.hasOrientation) {
        return;
    }
    bool ok = false;
    const QTransform inv = t.inverted(&ok);
    if (!ok) {
        return;
    }
    auto patientDir = [&](const QPointF& screenDelta) {
        const QPointF d = inv.map(screenDelta) - inv.map(QPointF(0, 0));
        const double sx = g.hasSpacing() ? g.spacingX : 1.0;
        const double sy = g.hasSpacing() ? g.spacingY : 1.0;
        return g.rowDir * (d.x() * sx) + g.colDir * (d.y() * sy);
    };
    const Vec3 right = patientDir({1, 0});
    const Vec3 down = patientDir({0, 1});
    QFont f = p.font();
    f.setPixelSize(14);
    f.setBold(true);
    p.save();
    p.setFont(f);
    const QFontMetricsF fm(f);
    const QColor c(0xF0, 0xD7, 0x8C);
    auto put = [&](const QString& s, QPointF pos) {
        drawOutlinedText(p, pos, s, c);
    };
    const QString r = QString::fromStdString(orientationLetters(right));
    const QString l = QString::fromStdString(orientationLetters(-right));
    const QString d = QString::fromStdString(orientationLetters(down));
    const QString u = QString::fromStdString(orientationLetters(-down));
    const double cy = size.height() / 2.0 + fm.ascent() / 2.0;
    put(l, QPointF(8, cy));
    put(r, QPointF(size.width() - 8 - fm.horizontalAdvance(r), cy));
    put(u, QPointF((size.width() - fm.horizontalAdvance(u)) / 2.0, 8 + fm.ascent() + 16));
    put(d, QPointF((size.width() - fm.horizontalAdvance(d)) / 2.0, size.height() - 8 - fm.descent() - 16));
    p.restore();
}

QImage Viewport::renderImage(bool withOverlays, bool withAnnotations, bool hidePatientData) const {
    const qreal dpr = devicePixelRatioF();
    QImage img((QSizeF(size()) * dpr).toSize(), QImage::Format_RGB32);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::black);
    QPainter p(&img);
    hidePatient_ = hidePatientData;
    paintContent(p, size(), withOverlays, withAnnotations, false);
    hidePatient_ = false;
    p.end();
    return img;
}

void Viewport::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

// ------------------------------------------------------- MPR guide handles

bool Viewport::guidesUsable(Tool tool) const {
    if (!mprCenter_ || mprGuides_.empty() || !shownFrame_) {
        return false;
    }
    // Navigation tools and the crosshair; measurement tools keep every click.
    switch (tool) {
        case Tool::Crosshair:
        case Tool::WindowLevel:
        case Tool::Pan:
        case Tool::Zoom:
        case Tool::Scroll: return true;
        default: return false;
    }
}

Viewport::GuideHit Viewport::hitGuide(const QPointF& pos) const {
    GuideHit hit;
    if (!mprCenter_ || mprGuides_.empty() || !shownFrame_) {
        return hit;
    }
    const QTransform t = transformFor(size());
    std::vector<GuideHandles> hs(mprGuides_.size());
    std::vector<bool> ok(mprGuides_.size(), false);
    for (size_t i = 0; i < mprGuides_.size(); ++i) {
        if (auto h = guideHandlesFor(i, t, size())) {
            hs[i] = *h;
            ok[i] = true;
        }
    }
    auto dist = [](const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); };
    const QPointF c = t.map(QPointF(mprCenter_->x, mprCenter_->y));
    if (dist(pos, c) <= 10.0) {
        hit.part = GuideHit::Center;
        return hit;
    }
    // Priority: rotation handles, slab handles, then the line itself.
    double best = 9.0;
    for (size_t i = 0; i < hs.size(); ++i) {
        for (int k = 0; ok[i] && k < 2; ++k) {
            if (const double d = dist(pos, hs[i].rotate[k]); d <= best) {
                best = d;
                hit = {GuideHit::Rotate, static_cast<int>(i), k == 0 ? 1 : -1};
            }
        }
    }
    if (hit.part != GuideHit::None) {
        return hit;
    }
    best = 9.0;
    for (size_t i = 0; i < hs.size(); ++i) {
        for (int k = 0; ok[i] && k < 2; ++k) {
            if (const double d = dist(pos, hs[i].slab[k]); d <= best) {
                best = d;
                hit = {GuideHit::Slab, static_cast<int>(i), k == 0 ? 1 : -1};
            }
        }
    }
    if (hit.part != GuideHit::None) {
        return hit;
    }
    best = 6.0;
    for (size_t i = 0; i < hs.size(); ++i) {
        if (!ok[i]) {
            continue;
        }
        const QPointF rel = pos - hs[i].center;
        const double d = std::abs(rel.x() * hs[i].dir.y() - rel.y() * hs[i].dir.x());
        if (d <= best) {
            best = d;
            hit = {GuideHit::Line, static_cast<int>(i), 0};
        }
    }
    return hit;
}

void Viewport::updateGuideCursor(const QPointF& pos) {
    const GuideHit h = guidesUsable(tool_) ? hitGuide(pos) : GuideHit{};
    if (h == hoverGuide_) {
        return;
    }
    hoverGuide_ = h;
    // Size cursor matching a movement across the line.
    auto across = [this](int guide) {
        const auto gh = guideHandles(static_cast<size_t>(guide));
        if (!gh) {
            return Qt::SizeAllCursor;
        }
        const double a = std::atan2(gh->normal.y(), gh->normal.x()) * 180.0 / std::numbers::pi;
        const double m = std::fmod(a + 180.0, 180.0);  // direction without sense, 0..180
        if (m < 22.5 || m >= 157.5) {
            return Qt::SizeHorCursor;
        }
        if (m < 67.5) {
            return Qt::SizeFDiagCursor;  // down-right (screen y grows downwards)
        }
        if (m < 112.5) {
            return Qt::SizeVerCursor;
        }
        return Qt::SizeBDiagCursor;
    };
    switch (h.part) {
        case GuideHit::Center: setCursor(Qt::SizeAllCursor); break;
        case GuideHit::Line: {
            const Qt::CursorShape s = across(h.guide);
            setCursor(s == Qt::SizeHorCursor ? Qt::SplitHCursor : s == Qt::SizeVerCursor ? Qt::SplitVCursor : s);
            break;
        }
        case GuideHit::Rotate: setCursor(rotateCursor_); break;
        case GuideHit::Slab: setCursor(across(h.guide)); break;
        case GuideHit::None: setCursor(cursorFor(tool_)); break;
    }
    update();
}

bool Viewport::beginGuideDrag(const QPointF& pos) {
    const GuideHit h = hitGuide(pos);
    if (h.part == GuideHit::None) {
        return false;
    }
    const QPointF c = transformFor(size()).map(QPointF(mprCenter_->x, mprCenter_->y));
    switch (h.part) {
        case GuideHit::Center:
            guideGrabOffset_ = pos - c;
            drag_ = Drag::MprCenter;
            break;
        case GuideHit::Line: {
            const auto gh = guideHandles(static_cast<size_t>(h.guide));
            if (!gh) {
                return false;
            }
            const QPointF rel = pos - c;
            guideGrabOffset_ = rel - gh->dir * QPointF::dotProduct(rel, gh->dir);  // across the line only
            drag_ = Drag::MprLine;
            break;
        }
        case GuideHit::Rotate:
            rotateLast_ = patientPointAt(pos);
            drag_ = Drag::MprRotate;
            break;
        case GuideHit::Slab: {
            const auto gh = guideHandles(static_cast<size_t>(h.guide));
            if (!gh) {
                return false;
            }
            const double half = mprGuides_[static_cast<size_t>(h.guide)].thickness / 2.0 * gh->pxPerMm;
            slabGrab_ = QPointF::dotProduct(pos - c, gh->normal) * h.side - half;
            drag_ = Drag::MprSlab;
            break;
        }
        case GuideHit::None: return false;
    }
    dragGuide_ = h;
    hoverGuide_ = h;
    return true;
}

void Viewport::continueGuideDrag(const QPointF& pos) {
    const int plane = dragGuide_.guide >= 0 && dragGuide_.guide < static_cast<int>(mprGuides_.size())
                          ? mprGuides_[static_cast<size_t>(dragGuide_.guide)].plane
                          : -1;
    switch (drag_) {
        case Drag::MprCenter:
            if (auto pp = patientPointAt(pos - guideGrabOffset_)) {
                Q_EMIT crosshairDragged(this, *pp);
            }
            break;
        case Drag::MprLine:
            if (auto pp = patientPointAt(pos - guideGrabOffset_); pp && plane >= 0) {
                Q_EMIT mprLineDragged(this, plane, *pp);
            }
            break;
        case Drag::MprRotate:
            if (auto pp = patientPointAt(pos)) {
                if (rotateLast_) {
                    Q_EMIT mprRotateDragged(this, *rotateLast_, *pp);
                }
                rotateLast_ = pp;
            }
            break;
        case Drag::MprSlab:
            if (const auto gh = guideHandles(static_cast<size_t>(std::max(0, dragGuide_.guide))); gh && plane >= 0) {
                const double across = QPointF::dotProduct(pos - gh->center, gh->normal) * dragGuide_.side - slabGrab_;
                Q_EMIT mprSlabDragged(this, plane, 2.0 * std::max(0.0, across) / gh->pxPerMm);
            }
            break;
        default: break;
    }
}

// ------------------------------------------------------------- interaction

Tool Viewport::toolForButton(Qt::MouseButton b, Qt::KeyboardModifiers mods) const {
    auto map = [this](MouseAction a) {
        switch (a) {
            case MouseAction::WindowLevel: return Tool::WindowLevel;
            case MouseAction::Pan: return Tool::Pan;
            case MouseAction::Zoom: return Tool::Zoom;
            case MouseAction::Scroll: return Tool::Scroll;
            default: return tool_;
        }
    };
    const auto& s = AppSettings::instance();
    if (b == Qt::LeftButton) {
        if (mods & Qt::ShiftModifier) {
            return Tool::Pan;
        }
        if (mods & Qt::ControlModifier) {
            return Tool::Zoom;
        }
        return map(s.leftButton());
    }
    if (b == Qt::MiddleButton) {
        return map(s.middleButton());
    }
    if (b == Qt::RightButton) {
        return map(s.rightButton());
    }
    return Tool::WindowLevel;
}

void Viewport::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    Q_EMIT activated(this);
    pressPos_ = lastPos_ = e->position();
    if (!source_ || !shownFrame_) {
        return;
    }
    const Tool t = toolForButton(e->button(), e->modifiers());
    beginDrag(t, e);
    update();
}

void Viewport::beginDrag(Tool tool, QMouseEvent* e) {
    const QPointF pos = e->position();
    const QTransform tr = transformFor(size());
    const Point2 ip = toImage(pos);
    const bool left = e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier));

    // Multi-step annotations waiting for the next click.
    if (left && pending_) {
        if (pending_->kind() == AnnotationKind::Angle && pending_->points.size() == 2) {
            pending_->points.push_back(ip);
            finishAnnotation();
            return;
        }
        if (pending_->kind() == AnnotationKind::Cobb && pending_->points.size() == 2) {
            pending_->points.push_back(ip);
            pending_->points.push_back(ip);
            editing_ = pending_;
            editingPoint_ = 3;
            drag_ = Drag::NewAnnotation;
            return;
        }
    }

    // MPR crosshair: lines and handles are dragged directly.
    if (left && !pending_ && guidesUsable(tool) && beginGuideDrag(pos)) {
        return;
    }

    // Editing existing annotations (any tool, left button).
    if (left && tool != Tool::Crosshair) {
        const auto& list = store_->list(annotationKey());
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            const AnnotationPtr& a = *it;
            const int h = a->hitPoint(pos, tr, 7.0);
            if (h >= 0 || a->hitLabel(pos) || a->hitBody(pos, tr, 5.0)) {
                setSelected(a);
                editing_ = a;
                editBefore_ = a->points;
                labelBefore_ = a->labelOffset;
                if (h >= 0) {
                    editingPoint_ = h;
                    drag_ = Drag::MovePoint;
                } else if (a->hitLabel(pos)) {
                    drag_ = Drag::MoveLabel;
                } else {
                    drag_ = Drag::MoveAnnotation;
                }
                return;
            }
        }
        setSelected(nullptr);
    }

    switch (tool) {
        case Tool::WindowLevel: drag_ = Drag::WindowLevel; break;
        case Tool::Pan:
            drag_ = Drag::Pan;
            setCursor(Qt::ClosedHandCursor);
            break;
        case Tool::Zoom: drag_ = Drag::Zoom; break;
        case Tool::Scroll:
            drag_ = Drag::Scroll;
            scrollAccumulator_ = 0.0;
            break;
        case Tool::Distance:
            startAnnotation(AnnotationKind::Distance, ip);
            drag_ = Drag::NewAnnotation;
            break;
        case Tool::Angle:
            startAnnotation(AnnotationKind::Angle, ip);
            drag_ = Drag::NewAnnotation;
            break;
        case Tool::Cobb:
            startAnnotation(AnnotationKind::Cobb, ip);
            drag_ = Drag::NewAnnotation;
            break;
        case Tool::RectRoi:
            startAnnotation(AnnotationKind::Rectangle, ip);
            drag_ = Drag::NewAnnotation;
            break;
        case Tool::EllipseRoi:
            startAnnotation(AnnotationKind::Ellipse, ip);
            drag_ = Drag::NewAnnotation;
            break;
        case Tool::FreehandRoi:
            startAnnotation(AnnotationKind::Freehand, ip);
            drag_ = Drag::Freehand;
            break;
        case Tool::Probe:
            probePos_ = pos;
            drag_ = Drag::Probe;
            break;
        case Tool::Crosshair:
            drag_ = Drag::Crosshair;
            if (auto pp = patientPointAt(pos)) {
                Q_EMIT crosshairDragged(this, *pp);
            }
            break;
    }
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    hoverPos_ = pos;
    updateCursorInfo(pos);
    if (drag_ == Drag::None) {
        if (pending_ && !pending_->points.empty()) {
            update();  // preview of the next segment is drawn from hoverPos_
        }
        if (mprCenter_ && (e->buttons() & Qt::LeftButton) == 0) {
            updateGuideCursor(pos);
        }
        return;
    }
    const QPointF delta = pos - lastPos_;
    lastPos_ = pos;
    switch (drag_) {
        case Drag::WindowLevel: {
            const double sens = std::clamp(width_ / 250.0, 0.1, 100.0);
            width_ = std::max(1.0, width_ + delta.x() * sens);
            center_ += delta.y() * sens;
            hasWindow_ = true;
            useVoiLut_ = false;
            Q_EMIT windowChanged(this);
            break;
        }
        case Drag::Pan:
            pan_ += delta;
            Q_EMIT viewTransformChanged(this);
            break;
        case Drag::Zoom: {
            const double current = fit_ ? fitZoom(size()) : zoom_;
            setZoom(current * std::exp(-delta.y() * 0.01), pressPos_);
            break;
        }
        case Drag::Scroll: {
            scrollAccumulator_ += delta.y();
            const int steps = static_cast<int>(scrollAccumulator_ / 6.0);
            if (steps != 0) {
                scrollAccumulator_ -= steps * 6.0;
                scrollBy(steps);
            }
            break;
        }
        case Drag::NewAnnotation:
        case Drag::MovePoint:
            if (editing_ && editingPoint_ >= 0 && editingPoint_ < static_cast<int>(editing_->points.size())) {
                editing_->points[static_cast<size_t>(editingPoint_)] = toImage(pos);
            }
            break;
        case Drag::Freehand:
            if (editing_) {
                const QTransform t = transformFor(size());
                const auto& last = editing_->points.back();
                const QPointF lastScreen = t.map(QPointF(last.x, last.y));
                if (std::hypot(lastScreen.x() - pos.x(), lastScreen.y() - pos.y()) > 2.5) {
                    editing_->points.push_back(toImage(pos));
                }
            }
            break;
        case Drag::MoveAnnotation:
            if (editing_) {
                const Point2 a = toImage(pos - delta);
                const Point2 b = toImage(pos);
                for (auto& pt : editing_->points) {
                    pt.x += b.x - a.x;
                    pt.y += b.y - a.y;
                }
            }
            break;
        case Drag::MoveLabel:
            if (editing_) {
                editing_->labelOffset += delta;
            }
            break;
        case Drag::Probe:
            probePos_ = pos;
            break;
        case Drag::Crosshair:
            if (auto pp = patientPointAt(pos)) {
                Q_EMIT crosshairDragged(this, *pp);
            }
            break;
        case Drag::MprCenter:
        case Drag::MprLine:
        case Drag::MprRotate:
        case Drag::MprSlab: continueGuideDrag(pos); break;
        case Drag::None:
            break;
    }
    update();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const double moved = std::hypot(pos.x() - pressPos_.x(), pos.y() - pressPos_.y());
    switch (drag_) {
        case Drag::NewAnnotation:
            if (editing_) {
                const auto kind = editing_->kind();
                if (kind == AnnotationKind::Angle && editing_->points.size() == 2) {
                    if (moved < 3.0) {
                        editing_.reset();
                    } else {
                        pending_ = editing_;
                        editing_.reset();
                    }
                } else if (kind == AnnotationKind::Cobb && editing_->points.size() == 2) {
                    if (moved < 3.0) {
                        editing_.reset();
                    } else {
                        pending_ = editing_;
                        editing_.reset();
                    }
                } else if (moved < 3.0 && kind != AnnotationKind::Cobb) {
                    editing_.reset();  // a click is not a measurement
                } else {
                    finishAnnotation();
                }
            }
            break;
        case Drag::Freehand:
            if (editing_) {
                if (editing_->points.size() >= 3) {
                    finishAnnotation();
                } else {
                    editing_.reset();
                }
            }
            break;
        case Drag::MovePoint:
        case Drag::MoveAnnotation:
        case Drag::MoveLabel:
            if (editing_ && (editing_->points.size() != editBefore_.size() || moved > 0.5)) {
                store_->recordEdit(annotationKey(), editing_, editBefore_, labelBefore_);
            }
            editing_.reset();
            break;
        case Drag::Probe:
            probePos_.reset();
            break;
        case Drag::Pan:
            setCursor(cursorFor(tool_));
            break;
        case Drag::MprCenter:
        case Drag::MprLine:
        case Drag::MprRotate:
        case Drag::MprSlab:
            dragGuide_ = {};
            rotateLast_.reset();
            break;
        default:
            break;
    }
    drag_ = Drag::None;
    if (mprCenter_) {
        hoverGuide_ = {GuideHit::None, -2, 0};  // force the cursor to be re-evaluated
        updateGuideCursor(pos);
    }
    update();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        cancelPendingAnnotation();
        Q_EMIT maximizeRequested(this);
    }
}

void Viewport::wheelEvent(QWheelEvent* e) {
    Q_EMIT activated(this);
    if (!source_) {
        return;
    }
    if (e->modifiers() & Qt::ControlModifier) {
        const double steps = e->angleDelta().y() / 120.0;
        const double current = fit_ ? fitZoom(size()) : zoom_;
        setZoom(current * std::pow(1.15, steps), e->position());
        e->accept();
        return;
    }
    // Mouse wheel: one image per notch. Trackpads send pixel deltas.
    if (!e->pixelDelta().isNull()) {
        wheelAccumulator_ += -e->pixelDelta().y() / 18.0;
    } else {
        wheelAccumulator_ += -e->angleDelta().y() / 120.0;
    }
    const int steps = static_cast<int>(wheelAccumulator_);
    if (steps != 0) {
        wheelAccumulator_ -= steps;
        scrollBy(steps);
    }
    e->accept();
}

void Viewport::keyPressEvent(QKeyEvent* e) {
    if (!source_) {
        QWidget::keyPressEvent(e);
        return;
    }
    const int page = std::max(1, source_->count() / 10);
    switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_Left: scrollBy(-1); break;
        case Qt::Key_Down:
        case Qt::Key_Right: scrollBy(1); break;
        case Qt::Key_PageUp: scrollBy(-page); break;
        case Qt::Key_PageDown: scrollBy(page); break;
        case Qt::Key_Home: setSliceIndex(0); break;
        case Qt::Key_End: setSliceIndex(source_->count() - 1); break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace: deleteSelectedAnnotation(); break;
        default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

void Viewport::leaveEvent(QEvent* event) {
    hoverPos_.reset();
    if (hoverGuide_.part != GuideHit::None && drag_ == Drag::None) {
        hoverGuide_ = {};
        setCursor(cursorFor(tool_));
        update();
    }
    Q_EMIT cursorInfo(QString());
    QWidget::leaveEvent(event);
}

void Viewport::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(kSeriesMime)) {
        e->acceptProposedAction();
    } else {
        e->ignore();  // file drops are handled by the main window
    }
}

void Viewport::dropEvent(QDropEvent* e) {
    if (e->mimeData()->hasFormat(kSeriesMime)) {
        const QString id = QString::fromUtf8(e->mimeData()->data(kSeriesMime));
        Q_EMIT seriesDropped(this, id);
        e->acceptProposedAction();
    }
}

void Viewport::updateCursorInfo(const QPointF& pos) {
    if (!source_ || !shownFrame_) {
        return;
    }
    const Point2 ip = toImage(pos);
    const int x = static_cast<int>(std::lround(ip.x));
    const int y = static_cast<int>(std::lround(ip.y));
    if (x < 0 || y < 0 || x >= shownFrame_->width || y >= shownFrame_->height) {
        Q_EMIT cursorInfo(QString());
        return;
    }
    QString text = tr("Pixel (%1, %2)").arg(x).arg(y);
    if (!shownFrame_->isColor()) {
        const double v = shownFrame_->valueAt(x, y);
        if (!std::isnan(v)) {
            const QString unit = source_->valueUnit();
            text += tr("   Valor: %1%2").arg(num(v, 1), unit.isEmpty() ? QString() : " " + unit);
        }
    }
    const auto g = source_->geometryAt(shownIndex_);
    if (g.isSpatial()) {
        const Vec3 p = g.pixelToPatient(ip.x, ip.y);
        text += tr("   Paciente: x %1  y %2  z %3 mm").arg(num(p.x, 1), num(p.y, 1), num(p.z, 1));
    }
    Q_EMIT cursorInfo(text);
}

}  // namespace vtc
