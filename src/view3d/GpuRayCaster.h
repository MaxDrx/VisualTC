#pragma once

#include <QImage>
#include <QString>
#include <cstdint>
#include <memory>

#include "render3d/Camera3D.h"
#include "render3d/RenderSettings.h"
#include "render3d/RenderVolume.h"
#include "render3d/TransferFunction.h"

namespace vtc {

// What the graphics card of this computer offers to the 3D view.
struct GpuCapabilities {
    bool usable = false;    // OpenGL 3.3 or newer, the shader compiles and draws
    bool software = false;  // emulated on the processor (no real graphics card)
    QString renderer;       // GL_RENDERER (card and driver), never patient data
    QString version;        // GL_VERSION
    QString reason;         // pt-BR, why it is not usable
    int max3DTexture = 0;   // largest side of a 3D texture
};

// Ray casting on the graphics card (OpenGL 3.3 core, GLSL shader), drawn in
// an offscreen framebuffer and read back as an image, so the same widget
// shows the CPU and GPU results and old drivers never touch the window
// system. Same rules as CpuRayCaster: empty-space skipping, early ray
// termination, shading, crop box, MIP/MinIP/mean and surface modes.
//
// Without OpenGL in the Qt build every call reports "not available" and the
// view uses the processor.
class GpuRayCaster {
public:
    GpuRayCaster();
    ~GpuRayCaster();
    GpuRayCaster(const GpuRayCaster&) = delete;
    GpuRayCaster& operator=(const GpuRayCaster&) = delete;

    // Tests the card once per run (context, version, shader, a small draw).
    static GpuCapabilities capabilities();
    static bool compiledIn();

    // Creates the context and the shader (GUI thread).
    bool initialize(QString* error);
    [[nodiscard]] bool ready() const;

    // Uploads the volume, reduced to the size limit of the card and to
    // `maxVoxels`; returns the volume actually on the card (nullptr: failed,
    // e.g. not enough video memory even after reducing).
    RenderVolumePtr setVolume(const RenderVolumePtr& volume, std::uint64_t maxVoxels, QString* error);
    [[nodiscard]] RenderVolumePtr volume() const;

    // Draws width x height pixels. `lut` must have been built for this volume
    // and for settings.stepMm(volume()->minSpacing()).
    bool render(const TransferLut& lut, const Camera3D& camera, const RenderSettings& settings, int width, int height,
                QImage* out, QString* error);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace vtc
