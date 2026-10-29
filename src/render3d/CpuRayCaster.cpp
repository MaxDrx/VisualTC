#include "render3d/CpuRayCaster.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "core/SystemInfo.h"

namespace vtc {

namespace {

constexpr int kTile = 16;
constexpr float kOpaqueEnough = 0.99F;
constexpr float kShadeContribution = 0.002F;  // same value in the shader of GpuRayCaster

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / std::max(1e-12F, e1 - e0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Repeatable per-pixel offset in [0, 1): breaks the rings of regular
// sampling without the noise changing from one frame to the next.
float pixelJitter(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093U ^ static_cast<std::uint32_t>(y) * 19349663U;
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFU) / 16777216.0F;
}

struct Context {
    const RenderVolume& vol;
    const TransferLut& lut;
    const Camera3D& cam;
    const RenderSettings& s;
    int width;
    int height;

    const std::uint16_t* data = nullptr;
    int nx = 0, ny = 0, nz = 0;
    std::size_t strideY = 0, strideZ = 0;
    Vec3 rowX, rowY, rowZ;  // patient -> index
    Vec3 origin;
    double boxLo[3] = {0, 0, 0};
    double boxHi[3] = {0, 0, 0};
    float step = 1.0F;  // mm
    float surfaceQ = 0.0F;
    double valueScale = 1.0;  // quantized -> modality units
    float gradScale[3] = {1, 1, 1};
    float shadeLow = 0.0F, shadeHigh = 1.0F;
    std::vector<std::uint8_t> useful;  // per block: can contribute to the image
    bool skipMaxima = true;            // maximum projection: skip blocks below the current maximum

    Context(const RenderVolume& v, const TransferLut& l, const Camera3D& c, const RenderSettings& st, int w, int h)
        : vol(v), lut(l), cam(c), s(st), width(w), height(h) {
        data = v.data();
        nx = v.nx();
        ny = v.ny();
        nz = v.nz();
        strideY = static_cast<std::size_t>(nx);
        strideZ = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
        rowX = v.axisX() / v.spacingX();
        rowY = v.axisY() / v.spacingY();
        rowZ = v.axisZ() / v.spacingZ();
        origin = v.origin();
        const int n[3] = {nx, ny, nz};
        for (int a = 0; a < 3; ++a) {
            const double lo = std::clamp(st.crop.lo[static_cast<std::size_t>(a)], 0.0, 1.0);
            const double hi = std::clamp(st.crop.hi[static_cast<std::size_t>(a)], lo, 1.0);
            boxLo[a] = lo * (n[a] - 1);
            boxHi[a] = hi * (n[a] - 1);
        }
        step = static_cast<float>(std::max(1e-3, st.stepMm(v.minSpacing())));
        surfaceQ = static_cast<float>(v.toQuantized(st.surfaceValue));
        valueScale = (v.maxValue() - v.minValue()) / 65535.0;
        gradScale[0] = static_cast<float>(valueScale / (2.0 * v.spacingX()));
        gradScale[1] = static_cast<float>(valueScale / (2.0 * v.spacingY()));
        gradScale[2] = static_cast<float>(valueScale / (2.0 * v.spacingZ()));
        // Shading fades in with the strength of the edge, relative to the
        // range of the data: homogeneous regions are not lit by noise.
        const auto range = static_cast<float>(v.maxValue() - v.minValue());
        shadeLow = 0.004F * range;
        shadeHigh = 0.03F * range;

        const int blocks = v.blocksX() * v.blocksY() * v.blocksZ();
        useful.assign(static_cast<std::size_t>(blocks), 1);
        skipMaxima = st.emptySpaceSkipping;
        if (st.emptySpaceSkipping && st.mode == RenderMode::Volume) {
            for (int b = 0; b < blocks; ++b) {
                useful[static_cast<std::size_t>(b)] = lut.anyOpaque(v.blockMin(b), v.blockMax(b)) ? 1 : 0;
            }
        } else if (st.emptySpaceSkipping && st.mode == RenderMode::Surface) {
            for (int b = 0; b < blocks; ++b) {
                useful[static_cast<std::size_t>(b)] = static_cast<float>(v.blockMax(b)) >= surfaceQ ? 1 : 0;
            }
        }
    }

    [[nodiscard]] float at(int x, int y, int z) const {
        return static_cast<float>(data[static_cast<std::size_t>(z) * strideZ + static_cast<std::size_t>(y) * strideY +
                                       static_cast<std::size_t>(x)]);
    }

    [[nodiscard]] float fetch(float x, float y, float z) const {
        x = std::clamp(x, 0.0F, static_cast<float>(nx - 1));
        y = std::clamp(y, 0.0F, static_cast<float>(ny - 1));
        z = std::clamp(z, 0.0F, static_cast<float>(nz - 1));
        const int x0 = std::min(static_cast<int>(x), nx - 1);
        const int y0 = std::min(static_cast<int>(y), ny - 1);
        const int z0 = std::min(static_cast<int>(z), nz - 1);
        const int x1 = std::min(x0 + 1, nx - 1);
        const int y1 = std::min(y0 + 1, ny - 1);
        const int z1 = std::min(z0 + 1, nz - 1);
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const float fz = z - static_cast<float>(z0);
        const std::size_t r00 = static_cast<std::size_t>(z0) * strideZ + static_cast<std::size_t>(y0) * strideY;
        const std::size_t r10 = static_cast<std::size_t>(z0) * strideZ + static_cast<std::size_t>(y1) * strideY;
        const std::size_t r01 = static_cast<std::size_t>(z1) * strideZ + static_cast<std::size_t>(y0) * strideY;
        const std::size_t r11 = static_cast<std::size_t>(z1) * strideZ + static_cast<std::size_t>(y1) * strideY;
        const auto ux0 = static_cast<std::size_t>(x0);
        const auto ux1 = static_cast<std::size_t>(x1);
        const float c00 = static_cast<float>(data[r00 + ux0]) +
                          (static_cast<float>(data[r00 + ux1]) - static_cast<float>(data[r00 + ux0])) * fx;
        const float c10 = static_cast<float>(data[r10 + ux0]) +
                          (static_cast<float>(data[r10 + ux1]) - static_cast<float>(data[r10 + ux0])) * fx;
        const float c01 = static_cast<float>(data[r01 + ux0]) +
                          (static_cast<float>(data[r01 + ux1]) - static_cast<float>(data[r01 + ux0])) * fx;
        const float c11 = static_cast<float>(data[r11 + ux0]) +
                          (static_cast<float>(data[r11 + ux1]) - static_cast<float>(data[r11 + ux0])) * fx;
        const float c0 = c00 + (c10 - c00) * fy;
        const float c1 = c01 + (c11 - c01) * fy;
        return c0 + (c1 - c0) * fz;
    }

    void lookup(float q, float out[4]) const {
        const float pos = q * static_cast<float>(TransferLut::kSize - 1) / 65535.0F;
        const int e = std::clamp(static_cast<int>(pos), 0, TransferLut::kSize - 2);
        const float f = std::clamp(pos - static_cast<float>(e), 0.0F, 1.0F);
        const float* a = lut.rgba.data() + static_cast<std::size_t>(e) * 4;
        const float* b = a + 4;
        for (int i = 0; i < 4; ++i) {
            out[i] = a[i] + (b[i] - a[i]) * f;
        }
    }

    // Gradient in modality units per mm, in patient coordinates.
    [[nodiscard]] Vec3 gradient(float x, float y, float z) const {
        const float gx = (fetch(x + 1.0F, y, z) - fetch(x - 1.0F, y, z)) * gradScale[0];
        const float gy = (fetch(x, y + 1.0F, z) - fetch(x, y - 1.0F, z)) * gradScale[1];
        const float gz = (fetch(x, y, z + 1.0F) - fetch(x, y, z - 1.0F)) * gradScale[2];
        return vol.axisX() * static_cast<double>(gx) + vol.axisY() * static_cast<double>(gy) +
               vol.axisZ() * static_cast<double>(gz);
    }

    // Lit colour; `strength` fades the lighting in homogeneous regions.
    void shade(float rgb[3], const Vec3& g, const Vec3& viewDir, bool always) const {
        const auto mag = static_cast<float>(g.norm());
        const float k = always ? (mag > 1e-6F ? 1.0F : 0.0F) : smoothstep(shadeLow, shadeHigh, mag);
        if (k <= 0.0F) {
            return;
        }
        const Vec3 n = g / static_cast<double>(mag);
        const float ndl = static_cast<float>(std::abs(n.dot(viewDir)));  // light at the eye, both faces lit
        const float spec = s.specular * std::pow(ndl, s.shininess);
        for (int i = 0; i < 3; ++i) {
            const float lit = rgb[i] * (s.ambient + s.diffuse * ndl) + spec;
            rgb[i] = std::min(1.0F, rgb[i] + (lit - rgb[i]) * k);
        }
    }

    [[nodiscard]] int blockOf(float x, float y, float z) const {
        const int bi = std::clamp(static_cast<int>(x) / RenderVolume::kBlock, 0, vol.blocksX() - 1);
        const int bj = std::clamp(static_cast<int>(y) / RenderVolume::kBlock, 0, vol.blocksY() - 1);
        const int bk = std::clamp(static_cast<int>(z) / RenderVolume::kBlock, 0, vol.blocksZ() - 1);
        return (bk * vol.blocksY() + bj) * vol.blocksX() + bi;
    }

    // Distance along the ray to leave the block that holds `p`.
    [[nodiscard]] static float blockExit(const float p[3], const float d[3]) {
        float best = std::numeric_limits<float>::max();
        for (int a = 0; a < 3; ++a) {
            if (std::abs(d[a]) < 1e-9F) {
                continue;
            }
            const float b = std::floor(p[a] / static_cast<float>(RenderVolume::kBlock));
            const float bound = (d[a] > 0.0F ? b + 1.0F : b) * static_cast<float>(RenderVolume::kBlock);
            best = std::min(best, (bound - p[a]) / d[a]);
        }
        return std::max(0.0F, best);
    }

    [[nodiscard]] float window(float q) const {
        const double v = vol.toValue(static_cast<double>(q));
        const double lo = s.windowCenter - 0.5 * s.windowWidth;
        return static_cast<float>(std::clamp((v - lo) / std::max(1e-9, s.windowWidth), 0.0, 1.0));
    }

    struct Hit {
        bool found = false;
        float x = 0, y = 0, z = 0;
    };

    // rgba: premultiplied colour and opacity of the ray (before the background).
    void cast(double px, double py, float jitter, float rgba[4], Hit* hit) const {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0F;
        const Ray r = cam.ray(px, py, width, height);
        const Vec3 rel = r.origin - origin;
        const float d[3] = {static_cast<float>(r.dir.dot(rowX)), static_cast<float>(r.dir.dot(rowY)),
                            static_cast<float>(r.dir.dot(rowZ))};
        // The origin may be far away (orthographic): clip in double precision.
        const double od[3] = {rel.dot(rowX), rel.dot(rowY), rel.dot(rowZ)};
        const double dd[3] = {r.dir.dot(rowX), r.dir.dot(rowY), r.dir.dot(rowZ)};
        double t0 = cam.perspective ? 0.0 : -std::numeric_limits<double>::max();
        double t1 = std::numeric_limits<double>::max();
        for (int a = 0; a < 3; ++a) {
            if (std::abs(dd[a]) < 1e-12) {
                if (od[a] < boxLo[a] || od[a] > boxHi[a]) {
                    return;
                }
                continue;
            }
            double ta = (boxLo[a] - od[a]) / dd[a];
            double tb = (boxHi[a] - od[a]) / dd[a];
            if (ta > tb) {
                std::swap(ta, tb);
            }
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        }
        if (t0 > t1) {
            return;
        }
        // Positions relative to the entry point keep float precision.
        const float base[3] = {static_cast<float>(od[0] + dd[0] * t0), static_cast<float>(od[1] + dd[1] * t0),
                               static_cast<float>(od[2] + dd[2] * t0)};
        const auto length = static_cast<float>(t1 - t0);
        const Vec3 viewDir = r.dir;
        float t = s.jitter ? jitter * step : 0.0F;

        auto position = [&](float tt, float p[3]) {
            for (int a = 0; a < 3; ++a) {
                p[a] = base[a] + d[a] * tt;
            }
        };

        switch (s.mode) {
            case RenderMode::Volume: {
                float transmit = 1.0F;
                while (t <= length) {
                    float p[3];
                    position(t, p);
                    if (!useful[static_cast<std::size_t>(blockOf(p[0], p[1], p[2]))]) {
                        const float leave = t + blockExit(p, d) + 1e-3F;
                        t += std::max(1.0F, std::ceil((leave - t) / step)) * step;
                        continue;
                    }
                    const float q = fetch(p[0], p[1], p[2]);
                    float c[4];
                    lookup(q, c);
                    if (c[3] > 1e-4F) {
                        // Lighting only where the sample shows: its gradient costs six reads.
                        if (s.shading && transmit * c[3] > kShadeContribution) {
                            shade(c, gradient(p[0], p[1], p[2]), viewDir, false);
                        }
                        const float w = transmit * c[3];
                        rgba[0] += w * c[0];
                        rgba[1] += w * c[1];
                        rgba[2] += w * c[2];
                        const float before = transmit;
                        transmit *= 1.0F - c[3];
                        if (hit != nullptr && !hit->found && before > 0.5F && transmit <= 0.5F) {
                            *hit = {true, p[0], p[1], p[2]};
                        }
                        if (transmit < 1.0F - kOpaqueEnough) {
                            break;
                        }
                    }
                    t += step;
                }
                rgba[3] = 1.0F - transmit;
                break;
            }
            case RenderMode::Surface: {
                float prevT = t;
                while (t <= length) {
                    float p[3];
                    position(t, p);
                    if (!useful[static_cast<std::size_t>(blockOf(p[0], p[1], p[2]))]) {
                        const float leave = t + blockExit(p, d) + 1e-3F;
                        prevT = t;
                        t += std::max(1.0F, std::ceil((leave - t) / step)) * step;
                        continue;
                    }
                    if (fetch(p[0], p[1], p[2]) >= surfaceQ) {
                        // Refine the crossing between the last two samples.
                        float lo = std::min(prevT, t);
                        float hi = t;
                        for (int it = 0; it < 6 && hi - lo > 1e-4F; ++it) {
                            const float mid = 0.5F * (lo + hi);
                            float pm[3];
                            position(mid, pm);
                            if (fetch(pm[0], pm[1], pm[2]) >= surfaceQ) {
                                hi = mid;
                            } else {
                                lo = mid;
                            }
                        }
                        position(hi, p);
                        float c[3] = {s.surfaceColor[0], s.surfaceColor[1], s.surfaceColor[2]};
                        if (s.shading) {
                            shade(c, gradient(p[0], p[1], p[2]), viewDir, true);
                        }
                        rgba[0] = c[0];
                        rgba[1] = c[1];
                        rgba[2] = c[2];
                        rgba[3] = 1.0F;
                        if (hit != nullptr) {
                            *hit = {true, p[0], p[1], p[2]};
                        }
                        return;
                    }
                    prevT = t;
                    t += step;
                }
                break;
            }
            case RenderMode::Mip:
            case RenderMode::MinIp:
            case RenderMode::Average: {
                const bool mip = s.mode == RenderMode::Mip;
                const bool minip = s.mode == RenderMode::MinIp;
                float best = mip ? -1.0F : 1e9F;
                float bestP[3] = {0, 0, 0};
                double sum = 0.0;
                int count = 0;
                while (t <= length) {
                    float p[3];
                    position(t, p);
                    if (mip && skipMaxima && static_cast<float>(vol.blockMax(blockOf(p[0], p[1], p[2]))) <= best) {
                        const float leave = t + blockExit(p, d) + 1e-3F;
                        t += std::max(1.0F, std::ceil((leave - t) / step)) * step;
                        continue;
                    }
                    const float q = fetch(p[0], p[1], p[2]);
                    if (mip) {
                        if (q > best) {
                            best = q;
                            std::copy(p, p + 3, bestP);
                        }
                    } else if (q >= 0.5F) {  // outside the scan and gaps do not count
                        if (minip) {
                            if (q < best) {
                                best = q;
                                std::copy(p, p + 3, bestP);
                            }
                        } else {
                            sum += static_cast<double>(q);
                            ++count;
                        }
                    }
                    t += step;
                }
                float value = 0.0F;
                if (mip) {
                    if (best < 0.0F) {
                        return;
                    }
                    value = window(best);
                } else if (minip) {
                    if (best > 1e8F) {
                        return;
                    }
                    value = window(best);
                } else {
                    if (count == 0) {
                        return;
                    }
                    value = window(static_cast<float>(sum / count));
                }
                rgba[0] = rgba[1] = rgba[2] = value;
                rgba[3] = 1.0F;
                if (hit != nullptr && (mip || minip)) {
                    *hit = {true, bestP[0], bestP[1], bestP[2]};
                }
                break;
            }
        }
    }
};

std::uint8_t toByte(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

}  // namespace

bool CpuRayCaster::render(const RenderVolume& volume, const TransferLut& lut, const Camera3D& camera,
                          const RenderSettings& settings, int width, int height, std::uint32_t* out,
                          const std::atomic<bool>* cancel, unsigned threads) {
    if (width <= 0 || height <= 0 || out == nullptr) {
        return true;
    }
    const Context ctx(volume, lut, camera, settings, width, height);
    const int tilesX = (width + kTile - 1) / kTile;
    const int tilesY = (height + kTile - 1) / kTile;
    const int tiles = tilesX * tilesY;
    std::atomic<int> next{0};
    std::atomic<bool> stopped{false};
    const auto& bg = settings.background;
    auto work = [&] {
        for (;;) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                stopped = true;
                return;
            }
            const int tile = next.fetch_add(1);
            if (tile >= tiles) {
                return;
            }
            const int x0 = (tile % tilesX) * kTile;
            const int y0 = (tile / tilesX) * kTile;
            const int x1 = std::min(width, x0 + kTile);
            const int y1 = std::min(height, y0 + kTile);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    float c[4];
                    ctx.cast(x + 0.5, y + 0.5, pixelJitter(x, y), c, nullptr);
                    const float keep = 1.0F - c[3];
                    out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                        0xFF000000U | (static_cast<std::uint32_t>(toByte(c[0] + keep * bg[0])) << 16) |
                        (static_cast<std::uint32_t>(toByte(c[1] + keep * bg[1])) << 8) |
                        static_cast<std::uint32_t>(toByte(c[2] + keep * bg[2]));
                }
            }
        }
    };
    const unsigned count =
        std::max(1U, std::min<unsigned>(threads == 0 ? hardwareThreads() : threads, static_cast<unsigned>(tiles)));
    std::vector<std::thread> pool;
    pool.reserve(count - 1);
    for (unsigned i = 1; i < count; ++i) {
        pool.emplace_back(work);
    }
    work();
    for (auto& th : pool) {
        th.join();
    }
    return !stopped.load();
}

std::optional<Vec3> CpuRayCaster::pick(const RenderVolume& volume, const TransferLut& lut, const Camera3D& camera,
                                       const RenderSettings& settings, int width, int height, double px, double py) {
    const Context ctx(volume, lut, camera, settings, width, height);
    float c[4];
    Context::Hit hit;
    ctx.cast(px, py, 0.0F, c, &hit);
    if (!hit.found) {
        return std::nullopt;
    }
    return volume.toPatient({hit.x, hit.y, hit.z});
}

}  // namespace vtc
