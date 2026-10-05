#include "mpr/Reslicer.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/Parallel.h"

namespace vtc {

FrameGeometry ReslicePlane::toFrameGeometry() const {
    FrameGeometry g;
    g.hasPosition = true;
    g.hasOrientation = true;
    g.position = origin;
    g.rowDir = u;
    g.colDir = v;
    g.spacingX = spacingU;
    g.spacingY = spacingV;
    g.spacingSource = SpacingSource::PixelSpacing;
    return g;
}

namespace {

// Samples one point using precomputed linear forms (no per-pixel Vec3 math).
inline double samplePoint(const ImageVolume& vol, double dn, double x, double y, Interpolation interp) {
    int k0 = 0;
    int k1 = 0;
    double t = 0.0;
    if (!vol.locateSlice(dn, k0, k1, t)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (interp == Interpolation::Nearest) {
        const int k = t < 0.5 ? k0 : k1;
        return vol.sliceSample(k, x - vol.originShiftX(k), y - vol.originShiftY(k), interp);
    }
    const double a = vol.sliceSample(k0, x - vol.originShiftX(k0), y - vol.originShiftY(k0), interp);
    if (k0 == k1 || t == 0.0) {
        return a;
    }
    const double b = vol.sliceSample(k1, x - vol.originShiftX(k1), y - vol.originShiftY(k1), interp);
    return a * (1.0 - t) + b * t;
}

}  // namespace

std::shared_ptr<DecodedFrame> reslice(const ImageVolume& vol, const ReslicePlane& plane, const SlabParams& slab,
                                      Interpolation interp) {
    auto out = std::make_shared<DecodedFrame>();
    out->width = std::max(0, plane.width);
    out->height = std::max(0, plane.height);
    out->format = PixelFormat::F32;
    out->slope = 1.0;
    out->intercept = 0.0;
    out->bitsStored = 32;
    out->data.resize(static_cast<std::size_t>(out->width) * static_cast<std::size_t>(out->height) * sizeof(float));
    if (out->width == 0 || out->height == 0 || vol.nz() == 0) {
        return out;
    }
    float* dst = out->as<float>();

    const Vec3 o0 = vol.origins().front();
    const Vec3& row = vol.rowDir();
    const Vec3& col = vol.colDir();
    const Vec3& nrm = vol.normal();
    const double sx = vol.spacingX();
    const double sy = vol.spacingY();
    const Vec3 du = plane.u * plane.spacingU;
    const Vec3 dv = plane.v * plane.spacingV;
    const Vec3 rel0 = plane.origin - o0;
    // Linear forms: value(i, j) = base + i * di + j * dj
    const double xBase = rel0.dot(row) / sx, xI = du.dot(row) / sx, xJ = dv.dot(row) / sx;
    const double yBase = rel0.dot(col) / sy, yI = du.dot(col) / sy, yJ = dv.dot(col) / sy;
    const double nBase = plane.origin.dot(nrm), nI = du.dot(nrm), nJ = dv.dot(nrm);

    // Slab sampling along the plane normal.
    const Vec3 pn = plane.normal();
    // Slab sampling step: the voxel spacing along the slab direction when it
    // is aligned with a volume axis (no information is lost by sampling
    // once per voxel), otherwise half the finest spacing.
    double step = std::min({vol.spacingX(), vol.spacingY(), vol.sliceSpacing()}) * 0.5;
    if (std::abs(pn.dot(vol.normal())) > 0.9999) {
        step = vol.sliceSpacing();
    } else if (std::abs(pn.dot(vol.rowDir())) > 0.9999) {
        step = vol.spacingX();
    } else if (std::abs(pn.dot(vol.colDir())) > 0.9999) {
        step = vol.spacingY();
    }
    step = std::max(0.05, step);
    int samples = 1;
    if (slab.thickness > 0.0) {
        samples = std::max(1, static_cast<int>(std::ceil(slab.thickness / step)) + 1);
        samples = std::min(samples, 2001);
    }
    std::vector<double> offX(static_cast<size_t>(samples));
    std::vector<double> offY(static_cast<size_t>(samples));
    std::vector<double> offN(static_cast<size_t>(samples));
    for (int s = 0; s < samples; ++s) {
        const double off = samples == 1 ? 0.0 : -slab.thickness / 2.0 + slab.thickness * s / (samples - 1);
        const Vec3 d = pn * off;
        offX[static_cast<size_t>(s)] = d.dot(row) / sx;
        offY[static_cast<size_t>(s)] = d.dot(col) / sy;
        offN[static_cast<size_t>(s)] = d.dot(nrm);
    }

    const int w = out->width;
    parallelFor(out->height, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j) {
            float* rowOut = dst + static_cast<std::ptrdiff_t>(j) * w;
            for (int i = 0; i < w; ++i) {
                const double x = xBase + i * xI + j * xJ;
                const double y = yBase + i * yI + j * yJ;
                const double dn = nBase + i * nI + j * nJ;
                if (samples == 1) {
                    rowOut[i] = static_cast<float>(samplePoint(vol, dn, x, y, interp));
                    continue;
                }
                double acc = 0.0;
                int n = 0;
                double best = slab.mode == SlabMode::MIP ? -std::numeric_limits<double>::infinity()
                                                         : std::numeric_limits<double>::infinity();
                for (int s = 0; s < samples; ++s) {
                    const auto su = static_cast<size_t>(s);
                    const double v = samplePoint(vol, dn + offN[su], x + offX[su], y + offY[su], interp);
                    if (std::isnan(v)) {
                        continue;
                    }
                    ++n;
                    if (slab.mode == SlabMode::Average) {
                        acc += v;
                    } else if (slab.mode == SlabMode::MIP) {
                        best = std::max(best, v);
                    } else {
                        best = std::min(best, v);
                    }
                }
                if (n == 0) {
                    rowOut[i] = std::numeric_limits<float>::quiet_NaN();
                } else {
                    rowOut[i] = static_cast<float>(slab.mode == SlabMode::Average ? acc / n : best);
                }
            }
        }
    });
    out->computeRange();
    return out;
}

}  // namespace vtc
