#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QPointF>
#include <QTimer>
#include <QWidget>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "render3d/Camera3D.h"
#include "render3d/RenderSettings.h"
#include "render3d/RenderVolume.h"
#include "render3d/Sculpt.h"
#include "render3d/TransferFunction.h"

namespace vtc {

class GpuRayCaster;

// Interactive 3D view: draws the volume with the graphics card when it can
// and with the processor otherwise. While the user drags, small fast images
// are drawn; when the mouse rests, the image is refined to full quality.
class VolumeRenderView : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Rotate, Pan, Zoom, Window, Scissors };
    enum class Backend { Automatic = 0, Gpu, Cpu };
    // Light: old computers (small volume, coarse sampling while moving).
    enum class Quality { Automatic = 0, Light, Balanced, Maximum };

    struct QualityParams {
        std::uint64_t volumeVoxels;       // resolution of the volume kept in memory
        std::uint64_t interactionVoxels;  // processor: reduced copy used while moving
        int interactionPixels;            // processor: image size while moving
        double finalStep;                 // graphics card: sampling distance at rest (voxels)
        double interactionStep;           // sampling distance while moving
        double cpuFinalStep;              // processor: sampling distance at rest
        double cpuFinalScale;             // processor: image size at rest (fraction of the view)
    };
    static QualityParams qualityParams(Quality q);
    // Quality chosen by "Automatic" for this computer and renderer.
    static Quality automaticQuality(bool gpu);

    explicit VolumeRenderView(QWidget* parent = nullptr);
    ~VolumeRenderView() override;

    void setVolume(const RenderVolumePtr& volume, bool keepCamera);
    [[nodiscard]] RenderVolumePtr volume() const { return volume_; }

    void setTransferFunction(const TransferFunction& tf);
    [[nodiscard]] const TransferFunction& transferFunction() const { return tf_; }
    void setOpacityScale(float scale);
    void setSettings(const RenderSettings& settings);
    [[nodiscard]] const RenderSettings& settings() const { return settings_; }

    // Tests the graphics card (once) and chooses where to render; before
    // this, the automatic quality assumes the processor.
    void prepareRenderer();
    void setBackend(Backend b);
    [[nodiscard]] Backend backend() const { return backend_; }
    [[nodiscard]] bool usingGpu() const { return gpuActive_; }
    void setQuality(Quality q);
    [[nodiscard]] Quality quality() const { return quality_; }
    [[nodiscard]] Quality effectiveQuality() const;
    [[nodiscard]] QString rendererDescription() const;

    void setTool(Tool t);
    [[nodiscard]] Tool tool() const { return tool_; }
    void setScissorsRemoveInside(bool inside) { removeInside_ = inside; }
    [[nodiscard]] bool scissorsRemoveInside() const { return removeInside_; }

    [[nodiscard]] const Camera3D& camera() const { return camera_; }
    void setView(ViewDirection v);
    void resetView();
    void rotateBy(double horizontalDeg, double verticalDeg, double rollDeg = 0.0);
    void setPerspective(bool on);
    void setSpinning(bool on);
    [[nodiscard]] bool spinning() const { return spinTimer_.isActive(); }
    void setCropBoxVisible(bool on);

    // Full quality image at the given size (device pixels), drawn now, of
    // the current view or of `camera` (kept in widget pixels); null when
    // there is no volume.
    QImage renderNow(int width, int height, const Camera3D* camera = nullptr);
    // What the view shows, at full quality, with the orientation cube.
    QImage snapshot();

    // The image on screen is the final one (full size and quality).
    [[nodiscard]] bool imageFinal() const { return imageFinal_ && !image_.isNull(); }
    // Time of the last image, for the status line.
    [[nodiscard]] double lastFrameMs() const { return lastFrameMs_; }

Q_SIGNALS:
    void cutDrawn(const vtc::SculptCut& cut);
    void transferFunctionDragged(const vtc::TransferFunction& tf);
    void settingsDragged(const vtc::RenderSettings& settings);
    void rendererChanged();
    void frameDrawn();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    struct Job {
        RenderVolumePtr volume;
        TransferFunction tf;
        float opacity = 1.0F;
        Camera3D camera;
        RenderSettings settings;
        int width = 0;
        int height = 0;
        bool final = false;
        quint64 generation = 0;
    };
    struct JobResult {
        QImage image;
        bool final = false;
        bool complete = false;
        quint64 generation = 0;
        double ms = 0.0;
    };

    void changed(bool interactive);
    void schedule();
    void renderGpu(bool final);
    void startCpu(bool final, double scale);
    void cpuFinished();
    void resolveBackend();
    void uploadToGpu();
    void prepareInteractionVolume();
    [[nodiscard]] Camera3D cameraFor(int width, int height) const;
    [[nodiscard]] QSize deviceSize() const;
    void drawOrientationCube(QPainter& p, const QRectF& area) const;
    void drawCropBox(QPainter& p) const;
    void beginInteraction();
    static JobResult runJob(const Job& job, const std::shared_ptr<std::atomic<bool>>& cancel);

    RenderVolumePtr volume_;
    RenderVolumePtr interactionVolume_;
    quint64 volumeSerial_ = 0;
    TransferFunction tf_;
    float opacity_ = 1.0F;
    RenderSettings settings_;
    Camera3D camera_;
    bool cameraFitted_ = false;

    Backend backend_ = Backend::Automatic;
    Quality quality_ = Quality::Automatic;
    bool gpuActive_ = false;
    bool backendResolved_ = false;
    QString gpuNote_;
    std::unique_ptr<GpuRayCaster> gpu_;
    quint64 gpuSerial_ = 0;
    double gpuScale_ = 1.0;

    Tool tool_ = Tool::Rotate;
    bool removeInside_ = true;
    enum class Drag { None, Rotate, Pan, Zoom, Window, Scissors };
    Drag drag_ = Drag::None;
    QPointF lastPos_;
    std::vector<QPointF> lasso_;
    TransferFunction dragTf_;
    RenderSettings dragSettings_;
    QPointF dragStart_;

    QImage image_;
    bool imageFinal_ = false;
    quint64 generation_ = 0;
    bool interacting_ = false;
    QTimer idleTimer_;
    QTimer spinTimer_;
    QTimer scheduleTimer_;
    bool pendingFinal_ = false;
    bool cropBoxVisible_ = false;
    double lastFrameMs_ = 0.0;

    QFutureWatcher<JobResult> cpuWatcher_;
    std::shared_ptr<std::atomic<bool>> cpuCancel_;
    bool cpuBusy_ = false;
    bool cpuPending_ = false;
    bool cpuPendingFinal_ = false;
    double cpuNextScale_ = 0.0;  // second pass of the progressive refinement
    QFutureWatcher<RenderVolumePtr> lodWatcher_;
};

}  // namespace vtc
