#include "mpr/ImageVolume.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace vtc {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool fitsInt16(const InstanceInfo& inst) {
    if (inst.samplesPerPixel != 1 || inst.bitsAllocated > 16) {
        return false;
    }
    if (inst.pixelRepresentation == 1) {
        return inst.bitsStored <= 16;
    }
    return inst.bitsStored <= 15;
}
}  // namespace

std::uint64_t ImageVolume::byteSize() const {
    return static_cast<std::uint64_t>(i16_.size()) * sizeof(std::int16_t) +
           static_cast<std::uint64_t>(f32_.size()) * sizeof(float);
}

ImageVolume::BuildResult ImageVolume::build(const Series& series, const FrameFetcher& fetch, std::uint64_t maxBytes,
                                            const Progress& progress, const std::atomic<bool>* cancel) {
    BuildResult res;
    const auto& g = series.geometry;
    if (!g.volumetric || series.frames.size() < 3) {
        res.error = "Não foi possível construir um volume espacial consistente a partir desta série.";
        return res;
    }
    auto vol = std::make_shared<ImageVolume>();
    vol->nx_ = g.columns;
    vol->ny_ = g.rows;
    vol->nz_ = static_cast<int>(series.frames.size());
    vol->rowDir_ = g.rowDir;
    vol->colDir_ = g.colDir;
    vol->normal_ = g.rowDir.cross(g.colDir).normalized();
    vol->sx_ = g.spacingX;
    vol->sy_ = g.spacingY;
    vol->seriesId_ = series.id;
    vol->frameOfReference_ = g.frameOfReferenceUid;
    vol->isCt_ = series.modality() == "CT";

    // Ascending order along the normal, whatever the display order.
    std::vector<size_t> order(series.frames.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&g](size_t a, size_t b) { return g.sliceOffsets[a] < g.sliceOffsets[b]; });

    // Storage type: int16 + common rescale when exact, else float32 values.
    const FrameInfo& f0 = series.frames.front().info();
    bool useInt16 = true;
    for (const auto& fr : series.frames) {
        const FrameInfo& fi = fr.info();
        if (!fitsInt16(*fr.instance) || fi.rescaleSlope != f0.rescaleSlope ||
            fi.rescaleIntercept != f0.rescaleIntercept) {
            useInt16 = false;
            break;
        }
    }
    const std::uint64_t voxels = static_cast<std::uint64_t>(vol->nx_) * static_cast<std::uint64_t>(vol->ny_) *
                                 static_cast<std::uint64_t>(vol->nz_);
    const std::uint64_t bytes = voxels * (useInt16 ? 2u : 4u);
    if (bytes > maxBytes) {
        res.error = "Memória insuficiente para montar o volume desta série (" + std::to_string(bytes / (1024 * 1024)) +
                    " MB necessários, limite " + std::to_string(maxBytes / (1024 * 1024)) + " MB).";
        return res;
    }
    try {
        if (useInt16) {
            vol->i16_.resize(voxels);
            vol->slope_ = f0.rescaleSlope;
            vol->intercept_ = f0.rescaleIntercept;
        } else {
            vol->f32_.resize(voxels);
            vol->isFloat_ = true;
        }
    } catch (const std::bad_alloc&) {
        res.error = "Memória insuficiente para montar o volume desta série.";
        return res;
    }

    const std::size_t sliceSize = static_cast<std::size_t>(vol->nx_) * static_cast<std::size_t>(vol->ny_);
    vol->origins_.reserve(order.size());
    for (size_t k = 0; k < order.size(); ++k) {
        if (cancel != nullptr && cancel->load()) {
            res.error = "Cancelado.";
            return res;
        }
        const FrameRef& ref = series.frames[order[k]];
        DecodedFramePtr frame = fetch(ref);
        if (!frame || frame->isColor() || frame->width != vol->nx_ || frame->height != vol->ny_) {
            res.error = "Falha ao carregar o corte " + std::to_string(order[k] + 1) + " para o volume.";
            return res;
        }
        vol->origins_.push_back(ref.geometry().position);
        const std::size_t base = k * sliceSize;
        if (useInt16) {
            for (std::size_t i = 0; i < sliceSize; ++i) {
                vol->i16_[base + i] = static_cast<std::int16_t>(frame->rawAt(i));
            }
        } else {
            for (std::size_t i = 0; i < sliceSize; ++i) {
                vol->f32_[base + i] = static_cast<float>(frame->rawAt(i) * frame->slope + frame->intercept);
            }
        }
        if (progress) {
            progress(static_cast<int>(k + 1), vol->nz_);
        }
    }
    vol->finalizeGeometry();
    res.volume = std::move(vol);
    return res;
}

std::shared_ptr<ImageVolume> ImageVolume::fromSlices(int nx, int ny, const Vec3& rowDir, const Vec3& colDir, double sx,
                                                     double sy, const std::vector<Vec3>& origins,
                                                     const std::vector<std::vector<float>>& slices) {
    auto vol = std::make_shared<ImageVolume>();
    vol->nx_ = nx;
    vol->ny_ = ny;
    vol->nz_ = static_cast<int>(slices.size());
    vol->rowDir_ = rowDir.normalized();
    vol->colDir_ = colDir.normalized();
    vol->normal_ = vol->rowDir_.cross(vol->colDir_).normalized();
    vol->sx_ = sx;
    vol->sy_ = sy;
    vol->isFloat_ = true;
    std::vector<size_t> order(slices.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return origins[a].dot(vol->normal_) < origins[b].dot(vol->normal_); });
    const std::size_t sliceSize = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    vol->f32_.resize(sliceSize * slices.size());
    for (size_t k = 0; k < order.size(); ++k) {
        vol->origins_.push_back(origins[order[k]]);
        std::copy(slices[order[k]].begin(), slices[order[k]].end(),
                  vol->f32_.begin() + static_cast<std::ptrdiff_t>(k * sliceSize));
    }
    vol->finalizeGeometry();
    return vol;
}

void ImageVolume::finalizeGeometry() {
    d_.resize(origins_.size());
    shiftX_.resize(origins_.size());
    shiftY_.resize(origins_.size());
    for (size_t k = 0; k < origins_.size(); ++k) {
        d_[k] = origins_[k].dot(normal_);
        const Vec3 delta = origins_[k] - origins_.front();
        shiftX_[k] = delta.dot(rowDir_) / sx_;
        shiftY_[k] = delta.dot(colDir_) / sy_;
    }
    std::vector<double> diffs;
    for (size_t k = 1; k < d_.size(); ++k) {
        diffs.push_back(d_[k] - d_[k - 1]);
    }
    if (diffs.empty()) {
        dz_ = std::min(sx_, sy_);
        uniform_ = true;
    } else {
        std::vector<double> sorted = diffs;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2),
                         sorted.end());
        dz_ = std::max(1e-6, sorted[sorted.size() / 2]);
        const double tol = std::max(0.01, 0.02 * dz_);
        uniform_ = std::all_of(diffs.begin(), diffs.end(), [&](double d) { return std::abs(d - dz_) <= tol; });
    }
    gapThreshold_ = 1.5 * dz_;
}

std::vector<Vec3> ImageVolume::corners() const {
    std::vector<Vec3> c;
    if (nz_ == 0) {
        return c;
    }
    const Vec3 ex = rowDir_ * ((nx_ - 1) * sx_);
    const Vec3 ey = colDir_ * ((ny_ - 1) * sy_);
    for (const Vec3& o : {origins_.front(), origins_.back()}) {
        c.push_back(o);
        c.push_back(o + ex);
        c.push_back(o + ey);
        c.push_back(o + ex + ey);
    }
    return c;
}

Vec3 ImageVolume::center() const {
    Vec3 acc;
    const auto c = corners();
    for (const auto& p : c) {
        acc += p;
    }
    return c.empty() ? acc : acc / static_cast<double>(c.size());
}

double ImageVolume::voxel(int x, int y, int z) const {
    const std::size_t idx = (static_cast<std::size_t>(z) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(y)) *
                                static_cast<std::size_t>(nx_) +
                            static_cast<std::size_t>(x);
    return isFloat_ ? static_cast<double>(f32_[idx]) : i16_[idx] * slope_ + intercept_;
}

bool ImageVolume::locateSlice(double dn, int& k0, int& k1, double& t) const {
    if (nz_ == 0) {
        return false;
    }
    const double half = 0.5 * dz_;
    if (dn < d_.front() - half || dn > d_.back() + half) {
        return false;
    }
    if (nz_ == 1 || dn <= d_.front()) {
        k0 = k1 = 0;
        t = 0.0;
        return true;
    }
    if (dn >= d_.back()) {
        k0 = k1 = nz_ - 1;
        t = 0.0;
        return true;
    }
    if (uniform_) {
        const double kf = (dn - d_.front()) / dz_;
        k0 = std::clamp(static_cast<int>(std::floor(kf)), 0, nz_ - 2);
        // Uniform test tolerates 2% jitter: refine to the true bracket.
        while (k0 > 0 && d_[static_cast<size_t>(k0)] > dn) {
            --k0;
        }
        while (k0 < nz_ - 2 && d_[static_cast<size_t>(k0) + 1] < dn) {
            ++k0;
        }
    } else {
        const auto it = std::upper_bound(d_.begin(), d_.end(), dn);
        k0 = static_cast<int>(std::distance(d_.begin(), it)) - 1;
        k0 = std::clamp(k0, 0, nz_ - 2);
    }
    k1 = k0 + 1;
    const double span = d_[static_cast<size_t>(k1)] - d_[static_cast<size_t>(k0)];
    if (span > gapThreshold_) {
        return false;  // inside a gap of missing slices
    }
    t = span > 0.0 ? (dn - d_[static_cast<size_t>(k0)]) / span : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    return true;
}

double ImageVolume::sliceSample(int k, double x, double y, Interpolation interp) const {
    if (x < -0.5 || y < -0.5 || x > nx_ - 0.5 || y > ny_ - 0.5) {
        return kNaN;
    }
    const std::size_t sliceBase = static_cast<std::size_t>(k) * static_cast<std::size_t>(nx_) *
                                  static_cast<std::size_t>(ny_);
    auto at = [&](int xi, int yi) -> double {
        const std::size_t idx =
            sliceBase + static_cast<std::size_t>(yi) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(xi);
        return isFloat_ ? static_cast<double>(f32_[idx]) : static_cast<double>(i16_[idx]);
    };
    double v = 0.0;
    if (interp == Interpolation::Nearest) {
        const int xi = std::clamp(static_cast<int>(std::lround(x)), 0, nx_ - 1);
        const int yi = std::clamp(static_cast<int>(std::lround(y)), 0, ny_ - 1);
        v = at(xi, yi);
    } else {
        x = std::clamp(x, 0.0, static_cast<double>(nx_ - 1));
        y = std::clamp(y, 0.0, static_cast<double>(ny_ - 1));
        const int x0 = static_cast<int>(x);
        const int y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, nx_ - 1);
        const int y1 = std::min(y0 + 1, ny_ - 1);
        const double fx = x - x0;
        const double fy = y - y0;
        const double top = at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx;
        const double bottom = at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx;
        v = top * (1.0 - fy) + bottom * fy;
    }
    return isFloat_ ? v : v * slope_ + intercept_;
}

double ImageVolume::sample(const Vec3& p, Interpolation interp) const {
    int k0 = 0;
    int k1 = 0;
    double t = 0.0;
    if (!locateSlice(p.dot(normal_), k0, k1, t)) {
        return kNaN;
    }
    const Vec3 rel = p - origins_.front();
    const double x = rel.dot(rowDir_) / sx_;
    const double y = rel.dot(colDir_) / sy_;
    if (interp == Interpolation::Nearest) {
        const int k = t < 0.5 ? k0 : k1;
        return sliceSample(k, x - shiftX_[static_cast<size_t>(k)], y - shiftY_[static_cast<size_t>(k)], interp);
    }
    const double a = sliceSample(k0, x - shiftX_[static_cast<size_t>(k0)], y - shiftY_[static_cast<size_t>(k0)], interp);
    if (k0 == k1 || t == 0.0) {
        return a;
    }
    const double b = sliceSample(k1, x - shiftX_[static_cast<size_t>(k1)], y - shiftY_[static_cast<size_t>(k1)], interp);
    return a * (1.0 - t) + b * t;
}

}  // namespace vtc
