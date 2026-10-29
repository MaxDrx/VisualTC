#include "view3d/GpuRayCaster.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(VISUALTC_HAVE_OPENGL)
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLVertexArrayObject>
#include <QSurfaceFormat>
#include <QVector2D>
#include <QVector3D>
#endif

#include "core/Logger.h"

namespace vtc {

#if defined(VISUALTC_HAVE_OPENGL)

namespace {

// Full-screen triangle: no vertex buffers needed.
const char* kVertexShader = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Same rules as CpuRayCaster (src/render3d/CpuRayCaster.cpp): keep both in step.
const char* kFragmentShader = R"(#version 330 core
out vec4 fragColor;

uniform sampler3D uVolume;   // R16, normalized quantized values
uniform sampler2D uLut;      // 4096 x 1 RGBA32F, opacity per sample
uniform sampler3D uUseful;   // R8 per 8^3 block: may contribute
uniform sampler3D uBlockMax; // R16 per block: largest value
uniform vec2 uSize;
uniform vec3 uTarget;        // patient mm, relative to the volume origin
uniform vec3 uRight;
uniform vec3 uDown;
uniform vec3 uForward;
uniform vec3 uEye;
uniform float uMmPerPixel;
uniform int uPerspective;
uniform vec3 uRowX;          // patient -> index
uniform vec3 uRowY;
uniform vec3 uRowZ;
uniform vec3 uAxisX;         // index axes in patient space
uniform vec3 uAxisY;
uniform vec3 uAxisZ;
uniform vec3 uDims;
uniform vec3 uBoxLo;
uniform vec3 uBoxHi;
uniform ivec3 uBlocks;
uniform float uStep;
uniform int uMode;           // 0 volume, 1 surface, 2 MIP, 3 MinIP, 4 mean
uniform int uShading;
uniform float uAmbient;
uniform float uDiffuse;
uniform float uSpecular;
uniform float uShininess;
uniform float uSurfaceQ;
uniform vec3 uSurfaceColor;
uniform float uWinLo;
uniform float uWinWidth;
uniform vec3 uGradScale;
uniform float uShadeLow;
uniform float uShadeHigh;
uniform vec3 uBackground;
uniform int uJitter;
uniform int uMaxSteps;
uniform int uSkipMaxima;

float pixelJitter(ivec2 p) {
    uint h = uint(p.x) * 73856093u ^ uint(p.y) * 19349663u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h & 0xFFFFFFu) / 16777216.0;
}

float sampleVolume(vec3 p) {
    return texture(uVolume, (clamp(p, vec3(0.0), uDims - 1.0) + 0.5) / uDims).r;
}

vec4 lookup(float v) {
    float pos = v * 4095.0;
    int e = clamp(int(pos), 0, 4094);
    float f = clamp(pos - float(e), 0.0, 1.0);
    return mix(texelFetch(uLut, ivec2(e, 0), 0), texelFetch(uLut, ivec2(e + 1, 0), 0), f);
}

vec3 gradientAt(vec3 p) {
    float gx = (sampleVolume(p + vec3(1.0, 0.0, 0.0)) - sampleVolume(p - vec3(1.0, 0.0, 0.0))) * uGradScale.x;
    float gy = (sampleVolume(p + vec3(0.0, 1.0, 0.0)) - sampleVolume(p - vec3(0.0, 1.0, 0.0))) * uGradScale.y;
    float gz = (sampleVolume(p + vec3(0.0, 0.0, 1.0)) - sampleVolume(p - vec3(0.0, 0.0, 1.0))) * uGradScale.z;
    return uAxisX * gx + uAxisY * gy + uAxisZ * gz;
}

vec3 shade(vec3 rgb, vec3 g, vec3 viewDir, bool always) {
    float mag = length(g);
    float k = always ? (mag > 1e-6 ? 1.0 : 0.0) : smoothstep(uShadeLow, uShadeHigh, mag);
    if (k <= 0.0) {
        return rgb;
    }
    float ndl = abs(dot(g / mag, viewDir));
    float spec = uSpecular * pow(ndl, uShininess);
    vec3 lit = rgb * (uAmbient + uDiffuse * ndl) + vec3(spec);
    return min(vec3(1.0), rgb + (lit - rgb) * k);
}

ivec3 blockOf(vec3 p) {
    return clamp(ivec3(p) / 8, ivec3(0), uBlocks - 1);
}

float blockExit(vec3 p, vec3 d) {
    float best = 1e30;
    for (int a = 0; a < 3; ++a) {
        if (abs(d[a]) < 1e-9) {
            continue;
        }
        float b = floor(p[a] / 8.0);
        float bound = (d[a] > 0.0 ? b + 1.0 : b) * 8.0;
        best = min(best, (bound - p[a]) / d[a]);
    }
    return max(0.0, best);
}

float skipStep(vec3 p, vec3 d) {
    return max(1.0, ceil((blockExit(p, d) + 1e-3) / uStep)) * uStep;
}

float windowed(float v) {
    return clamp((v - uWinLo) / max(1e-9, uWinWidth), 0.0, 1.0);
}

void main() {
    vec2 pix = vec2(gl_FragCoord.x, uSize.y - gl_FragCoord.y);
    vec3 onPlane = uTarget + uRight * ((pix.x - 0.5 * uSize.x) * uMmPerPixel) +
                   uDown * ((pix.y - 0.5 * uSize.y) * uMmPerPixel);
    vec3 ro = uPerspective != 0 ? uEye : onPlane;
    vec3 rd = uPerspective != 0 ? normalize(onPlane - uEye) : uForward;
    vec3 o = vec3(dot(uRowX, ro), dot(uRowY, ro), dot(uRowZ, ro));
    vec3 d = vec3(dot(uRowX, rd), dot(uRowY, rd), dot(uRowZ, rd));
    float t0 = uPerspective != 0 ? 0.0 : -1e30;
    float t1 = 1e30;
    for (int a = 0; a < 3; ++a) {
        if (abs(d[a]) < 1e-12) {
            if (o[a] < uBoxLo[a] || o[a] > uBoxHi[a]) {
                t0 = 1.0;
                t1 = 0.0;
            }
            continue;
        }
        float ta = (uBoxLo[a] - o[a]) / d[a];
        float tb = (uBoxHi[a] - o[a]) / d[a];
        t0 = max(t0, min(ta, tb));
        t1 = min(t1, max(ta, tb));
    }
    if (t0 > t1) {
        fragColor = vec4(uBackground, 1.0);
        return;
    }
    vec3 base = o + d * t0;
    float len = t1 - t0;
    float t = uJitter != 0 ? pixelJitter(ivec2(pix)) * uStep : 0.0;
    vec3 color = vec3(0.0);
    float alpha = 0.0;

    if (uMode == 0) {
        float transmit = 1.0;
        for (int i = 0; i < uMaxSteps && t <= len; ++i) {
            vec3 p = base + d * t;
            if (texelFetch(uUseful, blockOf(p), 0).r < 0.5) {
                t += skipStep(p, d);
                continue;
            }
            vec4 c = lookup(sampleVolume(p));
            if (c.a > 1e-4) {
                vec3 rgb = c.rgb;
                if (uShading != 0 && transmit * c.a > 0.002) {
                    rgb = shade(rgb, gradientAt(p), rd, false);
                }
                color += transmit * c.a * rgb;
                transmit *= 1.0 - c.a;
                if (transmit < 0.01) {
                    break;
                }
            }
            t += uStep;
        }
        alpha = 1.0 - transmit;
    } else if (uMode == 1) {
        float prevT = t;
        for (int i = 0; i < uMaxSteps && t <= len; ++i) {
            vec3 p = base + d * t;
            if (texelFetch(uUseful, blockOf(p), 0).r < 0.5) {
                prevT = t;
                t += skipStep(p, d);
                continue;
            }
            if (sampleVolume(p) >= uSurfaceQ) {
                float lo = min(prevT, t);
                float hi = t;
                for (int it = 0; it < 6 && hi - lo > 1e-4; ++it) {
                    float mid = 0.5 * (lo + hi);
                    if (sampleVolume(base + d * mid) >= uSurfaceQ) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                vec3 ph = base + d * hi;
                color = uShading != 0 ? shade(uSurfaceColor, gradientAt(ph), rd, true) : uSurfaceColor;
                alpha = 1.0;
                break;
            }
            prevT = t;
            t += uStep;
        }
    } else {
        bool mip = uMode == 2;
        bool minip = uMode == 3;
        float best = mip ? -1.0 : 1e9;
        float sum = 0.0;
        int count = 0;
        float empty = 0.5 / 65535.0;
        for (int i = 0; i < uMaxSteps && t <= len; ++i) {
            vec3 p = base + d * t;
            if (mip && uSkipMaxima != 0 && texelFetch(uBlockMax, blockOf(p), 0).r <= best) {
                t += skipStep(p, d);
                continue;
            }
            float v = sampleVolume(p);
            if (mip) {
                best = max(best, v);
            } else if (v >= empty) {
                if (minip) {
                    best = min(best, v);
                } else {
                    sum += v;
                    ++count;
                }
            }
            t += uStep;
        }
        bool found = mip ? best >= 0.0 : (minip ? best < 1e8 : count > 0);
        if (found) {
            float g = windowed(mip || minip ? best : sum / float(count));
            color = vec3(g);
            alpha = 1.0;
        }
    }
    fragColor = vec4(color + (1.0 - alpha) * uBackground, 1.0);
}
)";

bool isSoftwareRenderer(const QString& renderer) {
    const QString r = renderer.toLower();
    for (const char* s : {"llvmpipe", "softpipe", "swiftshader", "software", "gdi generic", "basic render", "swrast"}) {
        if (r.contains(QLatin1String(s))) {
            return true;
        }
    }
    return false;
}

// Pixels drawn per submission: short jobs for the card, so a slow card never
// reaches the driver's time limit (Windows resets cards stuck for 2 s).
constexpr int kBandPixels = 131072;

}  // namespace

struct GpuRayCaster::Impl {
    std::unique_ptr<QOpenGLContext> ctx;
    std::unique_ptr<QOffscreenSurface> surface;
    QOpenGLFunctions_3_3_Core* f = nullptr;
    std::unique_ptr<QOpenGLShaderProgram> program;
    std::unique_ptr<QOpenGLVertexArrayObject> vao;
    std::unique_ptr<QOpenGLFramebufferObject> fbo;
    GLuint volumeTex = 0;
    GLuint lutTex = 0;
    GLuint usefulTex = 0;
    GLuint blockMaxTex = 0;
    RenderVolumePtr volume;
    std::vector<std::uint8_t> useful;
    bool usefulUploaded = false;
    int max3D = 256;

    bool makeCurrent() { return ctx && surface && ctx->makeCurrent(surface.get()); }

    void deleteTextures() {
        for (GLuint* t : {&volumeTex, &lutTex, &usefulTex, &blockMaxTex}) {
            if (*t != 0) {
                f->glDeleteTextures(1, t);
                *t = 0;
            }
        }
    }

    ~Impl() {
        if (f != nullptr && makeCurrent()) {
            deleteTextures();
            fbo.reset();
            vao.reset();
            program.reset();
            ctx->doneCurrent();
        }
    }

    GLuint newTexture(GLenum target, GLint filter) {
        GLuint t = 0;
        f->glGenTextures(1, &t);
        f->glBindTexture(target, t);
        f->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
        f->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
        f->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (target == GL_TEXTURE_3D) {
            f->glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }
        return t;
    }

    void clearErrors() {
        for (int i = 0; i < 16 && f->glGetError() != GL_NO_ERROR; ++i) {
        }
    }
};

GpuRayCaster::GpuRayCaster() : d_(std::make_unique<Impl>()) {}

GpuRayCaster::~GpuRayCaster() = default;

bool GpuRayCaster::compiledIn() {
    return true;
}

bool GpuRayCaster::initialize(QString* error) {
    if (d_->program) {
        return true;
    }
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    d_->ctx = std::make_unique<QOpenGLContext>();
    d_->ctx->setFormat(fmt);
    if (!d_->ctx->create()) {
        *error = QCoreApplication::translate("GpuRayCaster", "Não foi possível criar um contexto OpenGL.");
        d_->ctx.reset();
        return false;
    }
    const QSurfaceFormat got = d_->ctx->format();
    if (got.version() < qMakePair(3, 3) || d_->ctx->isOpenGLES()) {
        *error =
            QCoreApplication::translate(
                "GpuRayCaster", "A placa de vídeo oferece OpenGL %1.%2; são necessários OpenGL 3.3 ou mais recente.")
                .arg(got.majorVersion())
                .arg(got.minorVersion());
        d_->ctx.reset();
        return false;
    }
    d_->surface = std::make_unique<QOffscreenSurface>();
    d_->surface->setFormat(got);
    d_->surface->create();
    if (!d_->makeCurrent()) {
        *error = QCoreApplication::translate("GpuRayCaster", "Não foi possível ativar o contexto OpenGL.");
        return false;
    }
    d_->f = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(d_->ctx.get());
    if (d_->f == nullptr || !d_->f->initializeOpenGLFunctions()) {
        *error = QCoreApplication::translate("GpuRayCaster", "Funções do OpenGL 3.3 indisponíveis neste driver.");
        d_->f = nullptr;
        return false;
    }
    auto program = std::make_unique<QOpenGLShaderProgram>();
    if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
        !program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) || !program->link()) {
        *error = QCoreApplication::translate("GpuRayCaster",
                                             "O driver da placa de vídeo não compilou o programa de renderização 3D.");
        logWarning("render3d", "Shader 3D não compilou: " + program->log().left(400).toStdString());
        return false;
    }
    d_->program = std::move(program);
    d_->vao = std::make_unique<QOpenGLVertexArrayObject>();
    d_->vao->create();
    GLint max3D = 256;
    d_->f->glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max3D);
    d_->max3D = std::clamp(max3D, 64, 2048);
    d_->clearErrors();
    return true;
}

bool GpuRayCaster::ready() const {
    return d_->program != nullptr;
}

RenderVolumePtr GpuRayCaster::volume() const {
    return d_->volume;
}

RenderVolumePtr GpuRayCaster::setVolume(const RenderVolumePtr& volume, std::uint64_t maxVoxels, QString* error) {
    if (!ready() || !volume || !d_->makeCurrent()) {
        *error = QCoreApplication::translate("GpuRayCaster", "Placa de vídeo indisponível.");
        return nullptr;
    }
    auto* f = d_->f;
    d_->deleteTextures();
    d_->volume.reset();
    d_->usefulUploaded = false;
    std::uint64_t budget = maxVoxels;
    for (int attempt = 0; attempt < 4; ++attempt) {
        RenderVolumePtr v = RenderVolume::reduced(volume, d_->max3D, budget);
        d_->clearErrors();
        d_->volumeTex = d_->newTexture(GL_TEXTURE_3D, GL_LINEAR);
        f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        f->glTexImage3D(GL_TEXTURE_3D, 0, GL_R16, v->nx(), v->ny(), v->nz(), 0, GL_RED, GL_UNSIGNED_SHORT, v->data());
        GLenum err = f->glGetError();
        if (err == GL_NO_ERROR) {
            // Largest value of each block (maximum projection skipping).
            std::vector<std::uint16_t> maxima(static_cast<std::size_t>(v->blocksX()) *
                                              static_cast<std::size_t>(v->blocksY()) *
                                              static_cast<std::size_t>(v->blocksZ()));
            for (std::size_t i = 0; i < maxima.size(); ++i) {
                maxima[i] = v->blockMax(static_cast<int>(i));
            }
            d_->blockMaxTex = d_->newTexture(GL_TEXTURE_3D, GL_NEAREST);
            f->glTexImage3D(GL_TEXTURE_3D, 0, GL_R16, v->blocksX(), v->blocksY(), v->blocksZ(), 0, GL_RED,
                            GL_UNSIGNED_SHORT, maxima.data());
            d_->usefulTex = d_->newTexture(GL_TEXTURE_3D, GL_NEAREST);
            d_->lutTex = d_->newTexture(GL_TEXTURE_2D, GL_NEAREST);
            err = f->glGetError();
        }
        if (err == GL_NO_ERROR) {
            d_->volume = v;
            return v;
        }
        d_->deleteTextures();
        if (err != GL_OUT_OF_MEMORY) {
            *error =
                QCoreApplication::translate("GpuRayCaster", "A placa de vídeo recusou o volume (erro OpenGL 0x%1).")
                    .arg(err, 0, 16);
            return nullptr;
        }
        // Not enough video memory: try again with a smaller volume.
        budget = std::max<std::uint64_t>(1ULL << 20, std::min<std::uint64_t>(budget, v->voxelCount()) / 2);
    }
    *error = QCoreApplication::translate("GpuRayCaster", "Memória de vídeo insuficiente para o volume.");
    return nullptr;
}

bool GpuRayCaster::render(const TransferLut& lut, const Camera3D& camera, const RenderSettings& s, int width,
                          int height, QImage* out, QString* error) {
    const RenderVolumePtr vol = d_->volume;
    if (!vol || width <= 0 || height <= 0 || !d_->makeCurrent()) {
        *error = QCoreApplication::translate("GpuRayCaster", "Placa de vídeo indisponível.");
        return false;
    }
    auto* f = d_->f;
    d_->clearErrors();
    if (!d_->fbo || d_->fbo->width() != width || d_->fbo->height() != height) {
        d_->fbo.reset();
        d_->fbo = std::make_unique<QOpenGLFramebufferObject>(width, height, QOpenGLFramebufferObject::NoAttachment,
                                                             GL_TEXTURE_2D, GL_RGBA8);
        if (!d_->fbo->isValid()) {
            d_->fbo.reset();
            *error = QCoreApplication::translate("GpuRayCaster",
                                                 "Não foi possível criar a área de desenho na placa de vídeo.");
            return false;
        }
    }
    // Blocks that can show something with these settings.
    const RenderVolume& v = *vol;
    const int blocks = v.blocksX() * v.blocksY() * v.blocksZ();
    std::vector<std::uint8_t> useful(static_cast<std::size_t>(blocks), 255);
    if (s.emptySpaceSkipping && s.mode == RenderMode::Volume) {
        for (int b = 0; b < blocks; ++b) {
            useful[static_cast<std::size_t>(b)] = lut.anyOpaque(v.blockMin(b), v.blockMax(b)) ? 255 : 0;
        }
    } else if (s.emptySpaceSkipping && s.mode == RenderMode::Surface) {
        const double q = v.toQuantized(s.surfaceValue);
        for (int b = 0; b < blocks; ++b) {
            useful[static_cast<std::size_t>(b)] = v.blockMax(b) >= q ? 255 : 0;
        }
    }
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!d_->usefulUploaded || useful != d_->useful) {
        f->glBindTexture(GL_TEXTURE_3D, d_->usefulTex);
        f->glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, v.blocksX(), v.blocksY(), v.blocksZ(), 0, GL_RED, GL_UNSIGNED_BYTE,
                        useful.data());
        d_->useful = std::move(useful);
        d_->usefulUploaded = true;
    }
    f->glBindTexture(GL_TEXTURE_2D, d_->lutTex);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, TransferLut::kSize, 1, 0, GL_RGBA, GL_FLOAT, lut.rgba.data());

    d_->fbo->bind();
    f->glViewport(0, 0, width, height);
    f->glDisable(GL_DEPTH_TEST);
    f->glDisable(GL_BLEND);
    auto* p = d_->program.get();
    p->bind();
    d_->vao->bind();
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_3D, d_->volumeTex);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, d_->lutTex);
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_3D, d_->usefulTex);
    f->glActiveTexture(GL_TEXTURE3);
    f->glBindTexture(GL_TEXTURE_3D, d_->blockMaxTex);
    f->glActiveTexture(GL_TEXTURE0);
    p->setUniformValue("uVolume", 0);
    p->setUniformValue("uLut", 1);
    p->setUniformValue("uUseful", 2);
    p->setUniformValue("uBlockMax", 3);

    auto vec = [](const Vec3& a) {
        return QVector3D(static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z));
    };
    const Vec3 origin = v.origin();
    p->setUniformValue("uSize", QVector2D(static_cast<float>(width), static_cast<float>(height)));
    p->setUniformValue("uTarget", vec(camera.target - origin));
    p->setUniformValue("uRight", vec(camera.right));
    p->setUniformValue("uDown", vec(camera.down));
    p->setUniformValue("uForward", vec(camera.forward));
    p->setUniformValue("uEye", vec(camera.eye(height) - origin));
    p->setUniformValue("uMmPerPixel", static_cast<float>(camera.mmPerPixel));
    p->setUniformValue("uPerspective", camera.perspective ? 1 : 0);
    p->setUniformValue("uRowX", vec(v.axisX() / v.spacingX()));
    p->setUniformValue("uRowY", vec(v.axisY() / v.spacingY()));
    p->setUniformValue("uRowZ", vec(v.axisZ() / v.spacingZ()));
    p->setUniformValue("uAxisX", vec(v.axisX()));
    p->setUniformValue("uAxisY", vec(v.axisY()));
    p->setUniformValue("uAxisZ", vec(v.axisZ()));
    p->setUniformValue("uDims",
                       QVector3D(static_cast<float>(v.nx()), static_cast<float>(v.ny()), static_cast<float>(v.nz())));
    const int n[3] = {v.nx(), v.ny(), v.nz()};
    float lo[3];
    float hi[3];
    for (int a = 0; a < 3; ++a) {
        const double l = std::clamp(s.crop.lo[static_cast<std::size_t>(a)], 0.0, 1.0);
        const double h = std::clamp(s.crop.hi[static_cast<std::size_t>(a)], l, 1.0);
        lo[a] = static_cast<float>(l * (n[a] - 1));
        hi[a] = static_cast<float>(h * (n[a] - 1));
    }
    p->setUniformValue("uBoxLo", QVector3D(lo[0], lo[1], lo[2]));
    p->setUniformValue("uBoxHi", QVector3D(hi[0], hi[1], hi[2]));
    f->glUniform3i(p->uniformLocation("uBlocks"), v.blocksX(), v.blocksY(), v.blocksZ());
    const double stepMm = std::max(1e-3, s.stepMm(v.minSpacing()));
    p->setUniformValue("uStep", static_cast<float>(stepMm));
    int mode = 0;
    switch (s.mode) {
        case RenderMode::Volume:
            mode = 0;
            break;
        case RenderMode::Surface:
            mode = 1;
            break;
        case RenderMode::Mip:
            mode = 2;
            break;
        case RenderMode::MinIp:
            mode = 3;
            break;
        case RenderMode::Average:
            mode = 4;
            break;
    }
    p->setUniformValue("uMode", mode);
    p->setUniformValue("uShading", s.shading ? 1 : 0);
    p->setUniformValue("uAmbient", s.ambient);
    p->setUniformValue("uDiffuse", s.diffuse);
    p->setUniformValue("uSpecular", s.specular);
    p->setUniformValue("uShininess", s.shininess);
    p->setUniformValue("uSurfaceQ", static_cast<float>(v.toQuantized(s.surfaceValue) / 65535.0));
    p->setUniformValue("uSurfaceColor", QVector3D(s.surfaceColor[0], s.surfaceColor[1], s.surfaceColor[2]));
    const double range = v.maxValue() - v.minValue();
    p->setUniformValue("uWinLo", static_cast<float>((s.windowCenter - 0.5 * s.windowWidth - v.minValue()) / range));
    p->setUniformValue("uWinWidth", static_cast<float>(s.windowWidth / range));
    p->setUniformValue("uGradScale", QVector3D(static_cast<float>(range / (2.0 * v.spacingX())),
                                               static_cast<float>(range / (2.0 * v.spacingY())),
                                               static_cast<float>(range / (2.0 * v.spacingZ()))));
    p->setUniformValue("uShadeLow", static_cast<float>(0.004 * range));
    p->setUniformValue("uShadeHigh", static_cast<float>(0.03 * range));
    p->setUniformValue("uBackground", QVector3D(s.background[0], s.background[1], s.background[2]));
    p->setUniformValue("uJitter", s.jitter ? 1 : 0);
    p->setUniformValue("uSkipMaxima", s.emptySpaceSkipping ? 1 : 0);
    const double diag = std::sqrt(static_cast<double>(v.nx()) * v.nx() * v.spacingX() * v.spacingX() +
                                  static_cast<double>(v.ny()) * v.ny() * v.spacingY() * v.spacingY() +
                                  static_cast<double>(v.nz()) * v.nz() * v.spacingZ() * v.spacingZ());
    p->setUniformValue("uMaxSteps", static_cast<int>(std::min(100000.0, diag / stepMm + 16.0)));

    f->glEnable(GL_SCISSOR_TEST);
    const int band = std::clamp(kBandPixels / width, 8, 256);
    for (int y = 0; y < height; y += band) {
        f->glScissor(0, y, width, std::min(band, height - y));
        f->glDrawArrays(GL_TRIANGLES, 0, 3);
        f->glFlush();
    }
    f->glDisable(GL_SCISSOR_TEST);
    d_->vao->release();
    p->release();
    const GLenum err = f->glGetError();
    QImage img = d_->fbo->toImage(true);
    d_->fbo->release();
    if (err != GL_NO_ERROR || img.isNull()) {
        *error = QCoreApplication::translate("GpuRayCaster", "Falha da placa de vídeo ao desenhar (erro OpenGL 0x%1).")
                     .arg(err, 0, 16);
        return false;
    }
    *out = img.convertToFormat(QImage::Format_RGB32);
    return true;
}

GpuCapabilities GpuRayCaster::capabilities() {
    static const GpuCapabilities caps = [] {
        GpuCapabilities c;
        GpuRayCaster g;
        QString error;
        if (!g.initialize(&error)) {
            c.reason = error;
            if (g.d_->ctx && g.d_->makeCurrent() && g.d_->f != nullptr) {
                c.renderer = QString::fromLatin1(reinterpret_cast<const char*>(g.d_->f->glGetString(GL_RENDERER)));
            }
            return c;
        }
        c.renderer = QString::fromLatin1(reinterpret_cast<const char*>(g.d_->f->glGetString(GL_RENDERER)));
        c.version = QString::fromLatin1(reinterpret_cast<const char*>(g.d_->f->glGetString(GL_VERSION)));
        c.max3DTexture = g.d_->max3D;
        c.software = isSoftwareRenderer(c.renderer);
        // A small sphere must come out of the shader as on the processor.
        const int n = 16;
        std::vector<float> values(static_cast<std::size_t>(n * n * n));
        for (int z = 0; z < n; ++z) {
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    const double r = std::sqrt((x - 7.5) * (x - 7.5) + (y - 7.5) * (y - 7.5) + (z - 7.5) * (z - 7.5));
                    values[static_cast<std::size_t>((z * n + y) * n + x)] = r < 5.0 ? 1000.0F : 0.0F;
                }
            }
        }
        const auto vol = RenderVolume::fromValues(n, n, n, 1.0, 1.0, 1.0, values, false);
        if (!g.setVolume(vol, 1ULL << 20, &error)) {
            c.reason = error;
            return c;
        }
        RenderSettings s;
        s.shading = false;
        TransferFunction tf;
        tf.points = {{0.0, 0, 0, 0, 0}, {400.0, 1, 1, 1, 0}, {600.0, 1, 1, 1, 0.9F}};
        const TransferLut lut = buildTransferLut(tf, *vol, s.stepMm(vol->minSpacing()));
        Camera3D cam;
        cam.setView(ViewDirection::Anterior);
        cam.fit(*vol, 32, 32);
        QImage img;
        if (!g.render(lut, cam, s, 32, 32, &img, &error)) {
            c.reason = error;
            return c;
        }
        const QRgb centre = img.pixel(16, 16);
        const QRgb corner = img.pixel(0, 0);
        if (qGray(centre) < 128 || qGray(corner) > 16) {
            c.reason = QCoreApplication::translate("GpuRayCaster",
                                                   "A placa de vídeo desenhou uma imagem incorreta no teste inicial.");
            return c;
        }
        c.usable = true;
        return c;
    }();
    return caps;
}

#else  // !VISUALTC_HAVE_OPENGL

struct GpuRayCaster::Impl {};

GpuRayCaster::GpuRayCaster() : d_(std::make_unique<Impl>()) {}
GpuRayCaster::~GpuRayCaster() = default;
bool GpuRayCaster::compiledIn() {
    return false;
}

GpuCapabilities GpuRayCaster::capabilities() {
    GpuCapabilities c;
    c.reason =
        QCoreApplication::translate("GpuRayCaster", "Esta versão do programa foi compilada sem suporte a OpenGL.");
    return c;
}

bool GpuRayCaster::initialize(QString* error) {
    *error = capabilities().reason;
    return false;
}

bool GpuRayCaster::ready() const {
    return false;
}

RenderVolumePtr GpuRayCaster::setVolume(const RenderVolumePtr& /*volume*/, std::uint64_t /*maxVoxels*/,
                                        QString* error) {
    *error = capabilities().reason;
    return nullptr;
}

RenderVolumePtr GpuRayCaster::volume() const {
    return nullptr;
}

bool GpuRayCaster::render(const TransferLut& /*lut*/, const Camera3D& /*camera*/, const RenderSettings& /*settings*/,
                          int /*width*/, int /*height*/, QImage* /*out*/, QString* error) {
    *error = capabilities().reason;
    return false;
}

#endif

}  // namespace vtc
