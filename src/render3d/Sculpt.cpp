#include "render3d/Sculpt.h"

#include <algorithm>
#include <cmath>

#include "core/Parallel.h"

namespace vtc {

std::vector<std::uint8_t> rasterizePolygon(const std::vector<std::array<double, 2>>& polygon, int width, int height) {
    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(std::max(0, width)) * static_cast<std::size_t>(std::max(0, height)), 0);
    if (polygon.size() < 3 || width <= 0 || height <= 0) {
        return mask;
    }
    std::vector<double> xs;
    const std::size_t n = polygon.size();
    for (int y = 0; y < height; ++y) {
        const double yc = y + 0.5;
        xs.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % n];
            if ((a[1] <= yc && yc < b[1]) || (b[1] <= yc && yc < a[1])) {
                xs.push_back(a[0] + (yc - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            // Pixels whose centre lies between the two crossings.
            const int x0 = std::max(0, static_cast<int>(std::ceil(xs[i] - 0.5)));
            const int x1 = std::min(width - 1, static_cast<int>(std::floor(xs[i + 1] - 0.5)));
            for (int x = x0; x <= x1; ++x) {
                mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 1;
            }
        }
    }
    return mask;
}

std::shared_ptr<RenderVolume> applyCuts(const RenderVolume& source, const std::vector<SculptCut>& cuts) {
    auto out = source.clone();
    if (cuts.empty()) {
        return out;
    }
    const int nx = out->nx();
    const int ny = out->ny();
    const int nz = out->nz();
    std::uint16_t* data = out->mutableData();
    for (const SculptCut& cut : cuts) {
        if (cut.polygon.size() < 3 || cut.width <= 0 || cut.height <= 0) {
            continue;
        }
        const auto mask = rasterizePolygon(cut.polygon, cut.width, cut.height);
        auto inside = [&](double px, double py) {
            const auto ix = static_cast<long long>(std::floor(px));
            const auto iy = static_cast<long long>(std::floor(py));
            if (ix < 0 || iy < 0 || ix >= cut.width || iy >= cut.height) {
                return false;
            }
            return mask[static_cast<std::size_t>(iy) * static_cast<std::size_t>(cut.width) +
                        static_cast<std::size_t>(ix)] != 0;
        };
        // Orthographic: pixel position linear in the voxel indices.
        const Camera3D& cam = cut.camera;
        double p0x = 0;
        double p0y = 0;
        const bool linear = !cam.perspective && cam.project(out->toPatient({0, 0, 0}), cut.width, cut.height, p0x, p0y);
        const Vec3 ex = out->axisX() * out->spacingX();
        const Vec3 ey = out->axisY() * out->spacingY();
        const Vec3 ez = out->axisZ() * out->spacingZ();
        const double inv = 1.0 / cam.mmPerPixel;
        const double dxi = ex.dot(cam.right) * inv;
        const double dyi = ex.dot(cam.down) * inv;
        const double dxj = ey.dot(cam.right) * inv;
        const double dyj = ey.dot(cam.down) * inv;
        const double dxk = ez.dot(cam.right) * inv;
        const double dyk = ez.dot(cam.down) * inv;
        parallelFor(nz, [&](int kb, int ke) {
            for (int k = kb; k < ke; ++k) {
                for (int j = 0; j < ny; ++j) {
                    std::uint16_t* row = data + (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                                                 static_cast<std::size_t>(j)) *
                                                    static_cast<std::size_t>(nx);
                    for (int i = 0; i < nx; ++i) {
                        if (row[i] == 0) {
                            continue;
                        }
                        double px = 0;
                        double py = 0;
                        bool in = false;
                        if (!linear) {
                            in = cam.project(out->toPatient({static_cast<double>(i), static_cast<double>(j),
                                                             static_cast<double>(k)}),
                                             cut.width, cut.height, px, py) &&
                                 inside(px, py);
                        } else {
                            px = p0x + i * dxi + j * dxj + k * dxk;
                            py = p0y + i * dyi + j * dyj + k * dyk;
                            in = inside(px, py);
                        }
                        if (in == cut.removeInside) {
                            row[i] = 0;
                        }
                    }
                }
            }
        });
    }
    out->refresh();
    return out;
}

}  // namespace vtc
