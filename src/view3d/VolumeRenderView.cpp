#include "view3d/VolumeRenderView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

#include "app/AppSettings.h"
#include "app/I18n.h"
#include "app/Theme.h"
#include "core/Logger.h"
#include "core/SystemInfo.h"
#include "render3d/CpuRayCaster.h"
#include "view3d/GpuRayCaster.h"

namespace vtc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kIdleMs = 220;
constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

double radians(double deg) {
    return deg * kPi / 180.0;
}

}  // namespace

VolumeRenderView::QualityParams VolumeRenderView::qualityParams(Quality q) {
    switch (q) {
        case Quality::Light:
            return {16ULL << 20, 2ULL << 20, 90'000, 0.6, 1.2, 0.8, 0.75};
        case Quality::Maximum: {
            // The full volume only where memory allows it.
            const std::uint64_t ram = physicalMemoryBytes();
            const std::uint64_t voxels =
                ram >= 12 * kGiB ? 192ULL << 20 : (ram >= 6 * kGiB ? 96ULL << 20 : 64ULL << 20);
            return {voxels, 12ULL << 20, 300'000, 0.33, 0.8, 0.5, 1.0};
        }
        case Quality::Automatic:
        case Quality::Balanced:
            break;
    }
    return {48ULL << 20, 6ULL << 20, 160'000, 0.5, 1.0, 0.6, 0.85};
}

VolumeRenderView::Quality VolumeRenderView::automaticQuality(bool gpu) {
    const std::uint64_t ram = physicalMemoryBytes();
    if (gpu) {
        return ram >= 8 * kGiB ? Quality::Maximum : Quality::Balanced;
    }
    return hardwareThreads() >= 8 && ram >= 8 * kGiB ? Quality::Balanced : Quality::Light;
}

VolumeRenderView::VolumeRenderView(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setMinimumSize(200, 200);
    idleTimer_.setSingleShot(true);
    idleTimer_.setInterval(kIdleMs);
    connect(&idleTimer_, &QTimer::timeout, this, [this] {
        interacting_ = false;
        if (!imageFinal_) {
            schedule();
        }
    });
    scheduleTimer_.setSingleShot(true);
    scheduleTimer_.setInterval(0);
    connect(&scheduleTimer_, &QTimer::timeout, this, [this] {
        if (!volume_) {
            return;
        }
        if (!backendResolved_) {
            resolveBackend();
        }
        const bool final = !interacting_;
        if (gpuActive_) {
            renderGpu(final);
        } else {
            startCpu(final, 0.0);
        }
    });
    spinTimer_.setInterval(33);
    connect(&spinTimer_, &QTimer::timeout, this, [this] {
        camera_.rotate(radians(2.0), 0.0);
        changed(true);
    });
    connect(&cpuWatcher_, &QFutureWatcher<JobResult>::finished, this, &VolumeRenderView::cpuFinished);
    connect(&lodWatcher_, &QFutureWatcher<RenderVolumePtr>::finished, this, [this] {
        const RenderVolumePtr lod = lodWatcher_.result();
        // Only for the volume it was made from.
        if (lod && lodWatcher_.property("serial").toULongLong() == volumeSerial_) {
            interactionVolume_ = lod;
        }
    });
    backend_ = static_cast<Backend>(std::clamp(AppSettings::instance().render3dBackend(), 0, 2));
    quality_ = static_cast<Quality>(std::clamp(AppSettings::instance().render3dQuality(), 0, 3));
}

VolumeRenderView::~VolumeRenderView() {
    if (cpuCancel_) {
        cpuCancel_->store(true);
    }
    cpuWatcher_.waitForFinished();
    lodWatcher_.waitForFinished();
}

void VolumeRenderView::setVolume(const RenderVolumePtr& volume, bool keepCamera) {
    volume_ = volume;
    ++volumeSerial_;
    interactionVolume_.reset();
    if (!volume_) {
        image_ = {};
        update();
        return;
    }
    if (!keepCamera || !cameraFitted_) {
        marker_.reset();
        camera_.setView(ViewDirection::Anterior);
        camera_.fit(*volume_, std::max(1, width()), std::max(1, height()));
        cameraFitted_ = width() > 1 && height() > 1;
    }
    if (backendResolved_) {
        if (gpuActive_) {
            uploadToGpu();
        }
        if (!gpuActive_) {
            prepareInteractionVolume();
        }
    }
    changed(true);
}

void VolumeRenderView::setTransferFunction(const TransferFunction& tf) {
    tf_ = tf;
    changed(true);
}

void VolumeRenderView::setOpacityScale(float scale) {
    opacity_ = std::clamp(scale, 0.0F, 4.0F);
    changed(true);
}

void VolumeRenderView::setSettings(const RenderSettings& settings) {
    settings_ = settings;
    changed(true);
}

void VolumeRenderView::setBackend(Backend b) {
    backend_ = b;
    AppSettings::instance().setRender3dBackend(static_cast<int>(b));
    backendResolved_ = false;
    gpu_.reset();
    gpuActive_ = false;
    resolveBackend();
    if (volume_) {
        changed(true);
    }
}

void VolumeRenderView::setQuality(Quality q) {
    quality_ = q;
    AppSettings::instance().setRender3dQuality(static_cast<int>(q));
    // The window rebuilds the volume at the new resolution and calls setVolume.
}

VolumeRenderView::Quality VolumeRenderView::effectiveQuality() const {
    if (quality_ != Quality::Automatic) {
        return quality_;
    }
    // Before prepareRenderer() the card is unknown: the safe choice.
    return automaticQuality(backendResolved_ && gpuActive_);
}

void VolumeRenderView::prepareRenderer() {
    if (!backendResolved_) {
        resolveBackend();
    }
}

QString VolumeRenderView::rendererDescription() const {
    if (gpuActive_) {
        const GpuCapabilities caps = GpuRayCaster::capabilities();
        return tr("Placa de vídeo: %1").arg(caps.renderer);
    }
    const QString cpu = tr("Processador: %n núcleo(s)", nullptr, static_cast<int>(hardwareThreads()));
    return gpuNote_.isEmpty() ? cpu : cpu + QStringLiteral(" — ") + gpuNote_;
}

void VolumeRenderView::resolveBackend() {
    backendResolved_ = true;
    gpuActive_ = false;
    gpuNote_.clear();
    if (backend_ != Backend::Cpu) {
        auto& settings = AppSettings::instance();
        if (settings.gpuTestRunning()) {
            // The previous test never finished: the driver closed the program.
            settings.setGpuTestRunning(false);
            settings.setGpuFailed(true);
        }
        if (backend_ == Backend::Gpu) {
            settings.setGpuFailed(false);  // the user asks for it: try again
        }
        if (backend_ == Backend::Automatic && settings.gpuFailed()) {
            gpuNote_ = tr("a placa de vídeo falhou em um uso anterior");
        } else {
            settings.setGpuTestRunning(true);
            const GpuCapabilities caps = GpuRayCaster::capabilities();
            settings.setGpuTestRunning(false);
            if (!caps.usable) {
                gpuNote_ = caps.reason;
            } else if (backend_ == Backend::Automatic && caps.software) {
                gpuNote_ = tr("sem aceleração de vídeo");
            } else {
                gpu_ = std::make_unique<GpuRayCaster>();
                QString error;
                if (gpu_->initialize(&error)) {
                    gpuActive_ = true;
                    uploadToGpu();
                } else {
                    gpuNote_ = error;
                    gpu_.reset();
                }
            }
        }
        if (gpuActive_) {
            logInfo("render3d", "3D na placa de vídeo: " + GpuRayCaster::capabilities().renderer.toStdString());
        } else if (!gpuNote_.isEmpty()) {
            logInfo("render3d", "3D no processador: " + gpuNote_.toStdString());
        }
    } else {
        logInfo("render3d", "3D no processador (escolha do usuário).");
    }
    if (!gpuActive_) {
        prepareInteractionVolume();
    }
    Q_EMIT rendererChanged();
}

void VolumeRenderView::uploadToGpu() {
    if (!gpu_ || !volume_) {
        return;
    }
    QString error;
    const RenderVolumePtr up = gpu_->setVolume(volume_, qualityParams(effectiveQuality()).volumeVoxels, &error);
    if (!up) {
        gpuActive_ = false;
        gpu_.reset();
        gpuNote_ = error;
        logWarning("render3d", "Placa de vídeo recusou o volume; 3D no processador.");
        prepareInteractionVolume();
        Q_EMIT rendererChanged();
        return;
    }
    gpuSerial_ = volumeSerial_;
    gpuScale_ = 1.0;
}

void VolumeRenderView::prepareInteractionVolume() {
    if (!volume_ || gpuActive_) {
        return;
    }
    const RenderVolumePtr vol = volume_;
    const std::uint64_t voxels = qualityParams(effectiveQuality()).interactionVoxels;
    if (vol->voxelCount() <= voxels) {
        interactionVolume_ = vol;
        return;
    }
    lodWatcher_.waitForFinished();
    lodWatcher_.setProperty("serial", static_cast<qulonglong>(volumeSerial_));
    lodWatcher_.setFuture(QtConcurrent::run([vol, voxels] { return RenderVolume::reduced(vol, 4096, voxels); }));
}

void VolumeRenderView::setTool(Tool t) {
    tool_ = t;
    lasso_.clear();
    switch (t) {
        case Tool::Rotate:
            setCursor(Qt::OpenHandCursor);
            break;
        case Tool::Pan:
            setCursor(Qt::SizeAllCursor);
            break;
        case Tool::Zoom:
            setCursor(Qt::SizeVerCursor);
            break;
        case Tool::Window:
            setCursor(Qt::ArrowCursor);
            break;
        case Tool::Scissors:
            setCursor(Qt::CrossCursor);
            break;
    }
    update();
}

void VolumeRenderView::setView(ViewDirection v) {
    camera_.setView(v);
    if (volume_) {
        camera_.fit(*volume_, std::max(1, width()), std::max(1, height()));
    }
    changed(true);
}

void VolumeRenderView::resetView() {
    setSpinning(false);
    setView(ViewDirection::Anterior);
}

void VolumeRenderView::rotateBy(double horizontalDeg, double verticalDeg, double rollDeg) {
    camera_.rotate(radians(horizontalDeg), radians(verticalDeg), radians(rollDeg));
    changed(true);
}

void VolumeRenderView::setPerspective(bool on) {
    camera_.perspective = on;
    changed(true);
}

void VolumeRenderView::setSpinning(bool on) {
    if (on) {
        spinTimer_.start();
    } else if (spinTimer_.isActive()) {
        spinTimer_.stop();
        changed(true);
    }
}

void VolumeRenderView::setCropBoxVisible(bool on) {
    cropBoxVisible_ = on;
    update();
}

QSize VolumeRenderView::deviceSize() const {
    const qreal dpr = devicePixelRatioF();
    return {std::max(1, static_cast<int>(std::lround(width() * dpr))),
            std::max(1, static_cast<int>(std::lround(height() * dpr)))};
}

Camera3D VolumeRenderView::cameraFor(int w, int h) const {
    Camera3D c = camera_;
    // The camera is kept in widget (logical) pixels; images of any size
    // show the same field.
    c.mmPerPixel = camera_.mmPerPixel * static_cast<double>(std::max(1, width())) / std::max(1, w);
    (void)h;
    return c;
}

void VolumeRenderView::changed(bool interactive) {
    ++generation_;
    imageFinal_ = false;
    if (interactive) {
        interacting_ = true;
        idleTimer_.start();
    }
    schedule();
}

void VolumeRenderView::schedule() {
    if (!scheduleTimer_.isActive()) {
        scheduleTimer_.start();
    }
}

void VolumeRenderView::renderGpu(bool final) {
    const RenderVolumePtr vol = gpu_ ? gpu_->volume() : nullptr;
    if (!vol) {
        gpuActive_ = false;
        prepareInteractionVolume();
        startCpu(final, 0.0);
        return;
    }
    const QualityParams q = qualityParams(effectiveQuality());
    const QSize dev = deviceSize();
    const double scale = final ? 1.0 : gpuScale_;
    const int w = std::max(1, static_cast<int>(std::lround(dev.width() * scale)));
    const int h = std::max(1, static_cast<int>(std::lround(dev.height() * scale)));
    RenderSettings s = settings_;
    s.stepVoxels = final ? q.finalStep : q.interactionStep;
    const TransferLut lut = buildTransferLut(tf_, *vol, s.stepMm(vol->minSpacing()), opacity_);
    QElapsedTimer timer;
    timer.start();
    QImage img;
    QString error;
    if (!gpu_->render(lut, cameraFor(w, h), s, w, h, &img, &error)) {
        logWarning("render3d", "Falha ao desenhar na placa de vídeo; 3D no processador.");
        gpuActive_ = false;
        gpu_.reset();
        gpuNote_ = error;
        prepareInteractionVolume();
        Q_EMIT rendererChanged();
        startCpu(final, 0.0);
        return;
    }
    lastFrameMs_ = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    if (!final) {
        // Keep the movement fluid on slower cards.
        if (lastFrameMs_ > 50.0) {
            gpuScale_ = std::max(0.3, gpuScale_ * 0.75);
        } else if (lastFrameMs_ < 18.0) {
            gpuScale_ = std::min(1.0, gpuScale_ * 1.15);
        }
    }
    image_ = img;
    imageFinal_ = final;
    update();
    Q_EMIT frameDrawn();
}

VolumeRenderView::JobResult VolumeRenderView::runJob(const Job& job, const std::shared_ptr<std::atomic<bool>>& cancel) {
    JobResult r;
    r.final = job.final;
    r.generation = job.generation;
    QElapsedTimer timer;
    timer.start();
    QImage img(job.width, job.height, QImage::Format_RGB32);
    const TransferLut lut =
        buildTransferLut(job.tf, *job.volume, job.settings.stepMm(job.volume->minSpacing()), job.opacity);
    r.complete = CpuRayCaster::render(*job.volume, lut, job.camera, job.settings, job.width, job.height,
                                      reinterpret_cast<std::uint32_t*>(img.bits()), cancel.get());
    r.image = img;
    r.ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    return r;
}

void VolumeRenderView::startCpu(bool final, double scaleOverride) {
    if (!volume_) {
        return;
    }
    if (cpuBusy_) {
        cpuPending_ = true;
        cpuPendingFinal_ = final;
        // A refinement in progress is out of date: stop it. Small images
        // while moving are left to finish, so something always appears.
        if (cpuCancel_ && property("cpuJobFinal").toBool()) {
            cpuCancel_->store(true);
        }
        return;
    }
    const QualityParams q = qualityParams(effectiveQuality());
    const QSize dev = deviceSize();
    const double pixels = static_cast<double>(dev.width()) * dev.height();
    double scale = 1.0;
    cpuNextScale_ = 0.0;
    if (scaleOverride > 0.0) {
        scale = scaleOverride;
    } else if (final) {
        // Progressive: a half-size image first when the full one is slow.
        scale = q.cpuFinalScale;
        if (pixels * scale * scale > 3.0 * q.interactionPixels) {
            cpuNextScale_ = scale;
            scale = 0.5 * scale;
        }
    } else {
        scale = std::min(1.0, std::sqrt(q.interactionPixels / std::max(1.0, pixels)));
    }
    Job job;
    job.final = final;
    job.volume = final ? volume_ : (interactionVolume_ ? interactionVolume_ : volume_);
    job.tf = tf_;
    job.opacity = opacity_;
    job.settings = settings_;
    job.settings.stepVoxels = final ? q.cpuFinalStep : q.interactionStep;
    job.settings.fastGradient = !final;
    job.width = std::max(1, static_cast<int>(std::lround(dev.width() * scale)));
    job.height = std::max(1, static_cast<int>(std::lround(dev.height() * scale)));
    job.camera = cameraFor(job.width, job.height);
    job.generation = generation_;
    cpuCancel_ = std::make_shared<std::atomic<bool>>(false);
    cpuBusy_ = true;
    setProperty("cpuJobFinal", final);
    setProperty("cpuJobScale", scale);
    auto cancel = cpuCancel_;
    cpuWatcher_.setFuture(QtConcurrent::run([job, cancel] { return runJob(job, cancel); }));
    update();  // "refining" note
}

void VolumeRenderView::cpuFinished() {
    cpuBusy_ = false;
    const JobResult r = cpuWatcher_.result();
    const bool current = r.generation == generation_;
    if (r.complete) {
        // An image a little out of date still beats an older one.
        image_ = r.image;
        const bool lastPass = cpuNextScale_ <= 0.0 || property("cpuJobScale").toDouble() >= cpuNextScale_ - 1e-9;
        imageFinal_ = current && r.final && lastPass;
        lastFrameMs_ = r.ms;
        update();
        Q_EMIT frameDrawn();
    }
    if (cpuPending_) {
        cpuPending_ = false;
        startCpu(!interacting_, 0.0);
        return;
    }
    if (r.complete && current && r.final && cpuNextScale_ > 0.0) {
        const double next = cpuNextScale_;
        startCpu(true, next);
        cpuNextScale_ = 0.0;
        return;
    }
    if (!r.complete || !current) {
        if (!interacting_) {
            schedule();
        }
    }
    update();
}

QImage VolumeRenderView::renderNow(int w, int h, const Camera3D* camera) {
    if (!volume_ || w <= 0 || h <= 0) {
        return {};
    }
    if (!backendResolved_) {
        resolveBackend();
    }
    const QualityParams q = qualityParams(effectiveQuality());
    Camera3D cam = camera != nullptr ? *camera : camera_;
    cam.mmPerPixel *= static_cast<double>(std::max(1, width())) / w;
    RenderSettings s = settings_;
    s.stepVoxels = q.finalStep;
    if (gpuActive_ && gpu_ && gpu_->volume()) {
        const RenderVolumePtr vol = gpu_->volume();
        const TransferLut lut = buildTransferLut(tf_, *vol, s.stepMm(vol->minSpacing()), opacity_);
        QImage img;
        QString error;
        if (gpu_->render(lut, cam, s, w, h, &img, &error)) {
            return img;
        }
    }
    s.stepVoxels = q.cpuFinalStep;
    QImage img(w, h, QImage::Format_RGB32);
    const TransferLut lut = buildTransferLut(tf_, *volume_, s.stepMm(volume_->minSpacing()), opacity_);
    CpuRayCaster::render(*volume_, lut, cam, s, w, h, reinterpret_cast<std::uint32_t*>(img.bits()));
    return img;
}

QImage VolumeRenderView::snapshot() {
    const QSize dev = deviceSize();
    QImage img = renderNow(dev.width(), dev.height());
    if (img.isNull()) {
        return img;
    }
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const double s = static_cast<double>(dev.width()) / std::max(1, width());
    p.scale(s, s);
    const double side = std::min(90.0, std::min(width(), height()) * 0.22);
    drawOrientationCube(p, QRectF(width() - side - 8, height() - side - 8, side, side));
    return img;
}

void VolumeRenderView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (volume_ && !cameraFitted_ && width() > 1 && height() > 1) {
        camera_.fit(*volume_, width(), height());
        cameraFitted_ = true;
    }
    if (volume_) {
        changed(true);
    }
}

void VolumeRenderView::paintEvent(QPaintEvent* /*e*/) {
    QPainter p(this);
    const auto& bg = settings_.background;
    p.fillRect(rect(), QColor::fromRgbF(bg[0], bg[1], bg[2]));
    if (!image_.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(rect()), image_, QRectF(image_.rect()));
    }
    p.setRenderHint(QPainter::Antialiasing);
    const QColor text = (bg[0] + bg[1] + bg[2]) > 1.5F ? QColor(20, 20, 20) : Theme::overlayText();
    if (!volume_) {
        p.setPen(text);
        p.drawText(rect(), Qt::AlignCenter, tr("Preparando a reconstrução 3D…"));
        return;
    }
    if (cropBoxVisible_) {
        drawCropBox(p);
    }
    const double side = std::min(90.0, std::min(width(), height()) * 0.22);
    drawOrientationCube(p, QRectF(width() - side - 8, height() - side - 8, side, side));
    if (marker_) {
        double mx = 0;
        double my = 0;
        if (camera_.project(*marker_, width(), height(), mx, my)) {
            p.setPen(QPen(Theme::measurement(), 1.6));
            p.drawLine(QPointF(mx - 9, my), QPointF(mx - 3, my));
            p.drawLine(QPointF(mx + 3, my), QPointF(mx + 9, my));
            p.drawLine(QPointF(mx, my - 9), QPointF(mx, my - 3));
            p.drawLine(QPointF(mx, my + 3), QPointF(mx, my + 9));
        }
    }
    if (lasso_.size() > 1) {
        QPainterPath path;
        path.moveTo(lasso_.front());
        for (std::size_t i = 1; i < lasso_.size(); ++i) {
            path.lineTo(lasso_[i]);
        }
        path.closeSubpath();
        QColor fill = Theme::colors().accent;
        fill.setAlpha(removeInside_ ? 60 : 20);
        p.fillPath(path, fill);
        p.setPen(QPen(Theme::colors().accent, 1.6, Qt::DashLine));
        p.drawPath(path);
    }
    if (cpuBusy_ && property("cpuJobFinal").toBool() && !gpuActive_) {
        QFont f = font();
        f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1.0));
        p.setFont(f);
        p.setPen(text);
        p.drawText(QRectF(8, height() - 24, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   tr("Refinando a imagem…"));
    }
}

void VolumeRenderView::drawCropBox(QPainter& p) const {
    if (!volume_) {
        return;
    }
    const auto& c = settings_.crop;
    const double n[3] = {volume_->nx() - 1.0, volume_->ny() - 1.0, volume_->nz() - 1.0};
    QPointF pts[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3 idx{((i & 1) != 0 ? c.hi[0] : c.lo[0]) * n[0], ((i & 2) != 0 ? c.hi[1] : c.lo[1]) * n[1],
                       ((i & 4) != 0 ? c.hi[2] : c.lo[2]) * n[2]};
        double x = 0;
        double y = 0;
        if (!camera_.project(volume_->toPatient(idx), width(), height(), x, y)) {
            return;
        }
        pts[i] = {x, y};
    }
    p.setPen(QPen(Theme::measurement(), 1.2));
    for (int i = 0; i < 8; ++i) {
        for (int bit : {1, 2, 4}) {
            if ((i & bit) == 0) {
                p.drawLine(pts[i], pts[i | bit]);
            }
        }
    }
}

void VolumeRenderView::drawOrientationCube(QPainter& p, const QRectF& area) const {
    // Faces named with the DICOM letters used on the 2D images.
    struct Face {
        Vec3 n;
        const char* label;
    };
    static const Face faces[6] = {{{1, 0, 0}, "L"},  {{-1, 0, 0}, "R"}, {{0, 1, 0}, "P"},
                                  {{0, -1, 0}, "A"}, {{0, 0, 1}, "H"},  {{0, 0, -1}, "F"}};
    const QPointF centre = area.center();
    const double scale = area.width() * 0.32;
    auto project = [&](const Vec3& v) {
        return QPointF(centre.x() + v.dot(camera_.right) * scale, centre.y() + v.dot(camera_.down) * scale);
    };
    struct Drawn {
        double depth;
        int index;
    };
    std::vector<Drawn> visible;
    for (int i = 0; i < 6; ++i) {
        const double facing = -faces[i].n.dot(camera_.forward);
        if (facing > 1e-3) {
            visible.push_back({facing, i});
        }
    }
    std::sort(visible.begin(), visible.end(), [](const Drawn& a, const Drawn& b) { return a.depth < b.depth; });
    QFont f = font();
    f.setBold(true);
    for (const Drawn& d : visible) {
        const Face& face = faces[d.index];
        // Two axes spanning the face.
        const Vec3 u = std::abs(face.n.x) > 0.5 ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        const Vec3 w = face.n.cross(u);
        QPolygonF poly;
        for (const auto& [a, b] : {std::pair{-1, -1}, std::pair{1, -1}, std::pair{1, 1}, std::pair{-1, 1}}) {
            poly << project(face.n + u * a + w * b);
        }
        const int shade = static_cast<int>(70 + 120 * d.depth);
        p.setPen(QPen(QColor(200, 200, 200, 200), 1.0));
        p.setBrush(QColor(shade / 2, shade / 2 + 10, shade / 2 + 25, 200));
        p.drawPolygon(poly);
        if (d.depth > 0.25) {
            const double size = std::max(6.0, scale * 0.9 * std::sqrt(d.depth));
            f.setPixelSize(static_cast<int>(size));
            p.setFont(f);
            p.setPen(QColor(240, 240, 240));
            const QPointF c = project(face.n);
            p.drawText(QRectF(c.x() - size, c.y() - size, 2 * size, 2 * size), Qt::AlignCenter,
                       QString::fromLatin1(face.label));
        }
    }
}

void VolumeRenderView::beginInteraction() {
    interacting_ = true;
    idleTimer_.start();
}

void VolumeRenderView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    lastPos_ = e->position();
    dragStart_ = e->position();
    drag_ = Drag::None;
    if (e->button() == Qt::LeftButton) {
        Tool t = tool_;
        if ((e->modifiers() & Qt::ControlModifier) != 0) {
            t = Tool::Window;
        } else if ((e->modifiers() & Qt::ShiftModifier) != 0 && t != Tool::Scissors) {
            t = Tool::Pan;
        }
        switch (t) {
            case Tool::Rotate:
                drag_ = Drag::Rotate;
                break;
            case Tool::Pan:
                drag_ = Drag::Pan;
                break;
            case Tool::Zoom:
                drag_ = Drag::Zoom;
                break;
            case Tool::Window:
                drag_ = Drag::Window;
                break;
            case Tool::Scissors:
                drag_ = Drag::Scissors;
                lasso_ = {e->position()};
                break;
        }
    } else if (e->button() == Qt::MiddleButton) {
        drag_ = Drag::Pan;
    } else if (e->button() == Qt::RightButton) {
        drag_ = Drag::Zoom;
    }
    if (drag_ == Drag::Window) {
        dragTf_ = tf_;
        dragSettings_ = settings_;
    }
    if (drag_ == Drag::Rotate) {
        setSpinning(false);
        setCursor(Qt::ClosedHandCursor);
    }
}

void VolumeRenderView::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const QPointF delta = pos - lastPos_;
    lastPos_ = pos;
    switch (drag_) {
        case Drag::None:
            return;
        case Drag::Rotate:
            camera_.rotate(delta.x() * 0.008, delta.y() * 0.008);
            break;
        case Drag::Pan:
            camera_.pan(delta.x(), delta.y());
            break;
        case Drag::Zoom:
            camera_.zoom(std::exp(-delta.y() * 0.01), dragStart_.x(), dragStart_.y(), width(), height());
            break;
        case Drag::Window: {
            if (!volume_) {
                return;
            }
            const QPointF total = pos - dragStart_;
            const double range = volume_->maxValue() - volume_->minValue();
            const double unit = volume_->isCt() ? 2.0 : range / 800.0;
            if (settings_.mode == RenderMode::Volume) {
                // Horizontal: moves the colours along the values; vertical:
                // narrows or widens the ramps (as a window).
                const double scale = std::exp(total.y() * 0.004);
                tf_ = dragTf_.transformed(total.x() * unit, scale, dragTf_.center());
                Q_EMIT transferFunctionDragged(tf_);
            } else if (settings_.mode == RenderMode::Surface) {
                settings_.surfaceValue = dragSettings_.surfaceValue + total.x() * unit;
                Q_EMIT settingsDragged(settings_);
            } else {
                const double sens = std::clamp(settings_.windowWidth / 250.0, range * 1e-5, range);
                settings_.windowWidth = std::max(range * 1e-4, settings_.windowWidth + delta.x() * sens);
                settings_.windowCenter += delta.y() * sens;
                Q_EMIT settingsDragged(settings_);
            }
            break;
        }
        case Drag::Scissors: {
            if (lasso_.empty() || std::hypot(pos.x() - lasso_.back().x(), pos.y() - lasso_.back().y()) > 2.5) {
                lasso_.push_back(pos);
                update();
            }
            return;
        }
    }
    changed(true);
}

void VolumeRenderView::mouseReleaseEvent(QMouseEvent* /*e*/) {
    if (drag_ == Drag::Scissors) {
        if (lasso_.size() >= 3) {
            SculptCut cut;
            cut.camera = camera_;
            cut.width = width();
            cut.height = height();
            cut.removeInside = removeInside_;
            for (const QPointF& pt : lasso_) {
                cut.polygon.push_back({pt.x(), pt.y()});
            }
            Q_EMIT cutDrawn(cut);
        }
        lasso_.clear();
        update();
    }
    if (drag_ == Drag::Rotate) {
        setTool(tool_);
    }
    drag_ = Drag::None;
}

std::optional<Vec3> VolumeRenderView::pick(const QPointF& pos) const {
    if (!volume_) {
        return std::nullopt;
    }
    RenderSettings s = settings_;
    s.stepVoxels = 0.5;
    const TransferLut lut = buildTransferLut(tf_, *volume_, s.stepMm(volume_->minSpacing()), opacity_);
    return CpuRayCaster::pick(*volume_, lut, camera_, s, std::max(1, width()), std::max(1, height()), pos.x(), pos.y());
}

void VolumeRenderView::setMarker(const std::optional<Vec3>& p) {
    marker_ = p;
    update();
}

void VolumeRenderView::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || tool_ == Tool::Scissors) {
        QWidget::mouseDoubleClickEvent(e);
        return;
    }
    const std::optional<Vec3> p = pick(e->position());
    setMarker(p);
    if (p) {
        Q_EMIT pointPicked(*p);
    }
}

void VolumeRenderView::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }
    camera_.zoom(std::pow(1.12, steps), e->position().x(), e->position().y(), width(), height());
    changed(true);
}

void VolumeRenderView::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Left:
            rotateBy(-5.0, 0.0);
            break;
        case Qt::Key_Right:
            rotateBy(5.0, 0.0);
            break;
        case Qt::Key_Up:
            rotateBy(0.0, -5.0);
            break;
        case Qt::Key_Down:
            rotateBy(0.0, 5.0);
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            camera_.zoom(1.12);
            changed(true);
            break;
        case Qt::Key_Minus:
            camera_.zoom(1.0 / 1.12);
            changed(true);
            break;
        case Qt::Key_Home:
            resetView();
            break;
        case Qt::Key_Escape:
            if (!lasso_.empty()) {
                lasso_.clear();
                drag_ = Drag::None;
                update();
            } else {
                QWidget::keyPressEvent(e);
            }
            break;
        default:
            QWidget::keyPressEvent(e);
            break;
    }
}

}  // namespace vtc
