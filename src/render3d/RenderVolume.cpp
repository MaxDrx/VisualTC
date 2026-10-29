#include "render3d/RenderVolume.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/Parallel.h"

namespace vtc {

namespace {

constexpr int kHistogramBins = 4096;

// Number of voxels of an axis of physical length `extent` sampled every `spacing`.
int axisCount(double extent, double spacing) {
    return std::max(1, static_cast<int>(std::floor(extent / spacing + 1e-6)) + 1);
}

std::uint64_t gridCount(const double extent[3], const double spacing[3]) {
    std::uint64_t n = 1;
    for (int a = 0; a < 3; ++a) {
        n *= static_cast<std::uint64_t>(axisCount(extent[a], spacing[a]));
    }
    return n;
}

// Spacings no finer than the source and as close to isotropic as the budget
// allows: thin axes are reduced first, so thick-slice exams keep their
// in-plane detail as long as possible.
void chooseSpacing(const double extent[3], const double source[3], std::uint64_t maxVoxels, double out[3]) {
    for (int a = 0; a < 3; ++a) {
        out[a] = source[a];
    }
    if (maxVoxels == 0 || gridCount(extent, out) <= maxVoxels) {
        return;
    }
    double lo = std::min({source[0], source[1], source[2]});
    double hi = std::max({extent[0], extent[1], extent[2], lo}) + lo;
    for (int it = 0; it < 60; ++it) {
        const double mid = 0.5 * (lo + hi);
        double s[3];
        for (int a = 0; a < 3; ++a) {
            s[a] = std::max(source[a], mid);
        }
        if (gridCount(extent, s) <= maxVoxels) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    for (int a = 0; a < 3; ++a) {
        out[a] = std::max(source[a], hi);
    }
}

// Area-weighted (box) resampling along one axis. Output sample o is centred
// at `offset + o * r` in source index units and covers a box of width
// max(r, 1); source sample s covers [s - 0.5, s + 0.5]. Each output keeps the
// sources it overlaps and by how much: averages without moiré when the two
// grids do not match, linear interpolation when r = 1.
struct Taps {
    std::vector<int> begin;  // nOut + 1
    std::vector<int> index;
    std::vector<float> weight;
};

Taps makeTaps(int nOut, int nSrc, double r, double offset) {
    Taps t;
    t.begin.reserve(static_cast<std::size_t>(nOut) + 1);
    t.begin.push_back(0);
    const double width = std::max(r, 1.0);
    for (int o = 0; o < nOut; ++o) {
        const double c = offset + o * r;
        const double a = c - 0.5 * width;
        const double b = c + 0.5 * width;
        const int s0 = std::max(0, static_cast<int>(std::floor(a + 0.5)));
        const int s1 = std::min(nSrc - 1, static_cast<int>(std::ceil(b - 0.5)));
        for (int src = s0; src <= s1; ++src) {
            const double overlap = std::min(b, src + 0.5) - std::max(a, src - 0.5);
            if (overlap > 1e-9) {
                t.index.push_back(src);
                t.weight.push_back(static_cast<float>(overlap));
            }
        }
        t.begin.push_back(static_cast<int>(t.index.size()));
    }
    return t;
}

// One source slice reduced to the output grid: weighted sums and the area
// that real data covered (outside the slice nothing is counted).
struct ReducedSlice {
    std::vector<float> sum;
    std::vector<float> cover;
};

// Two passes (rows, then columns) over a slice read through `value(x, y)`.
template <class ValueFn>
void reduceSlice(int srcNy, const Taps& tx, const Taps& ty, ValueFn value, ReducedSlice& out,
                 std::vector<float>& rowSum, std::vector<float>& rowCover) {
    const int nxOut = static_cast<int>(tx.begin.size()) - 1;
    const int nyOut = static_cast<int>(ty.begin.size()) - 1;
    rowSum.assign(static_cast<std::size_t>(srcNy) * static_cast<std::size_t>(nxOut), 0.0F);
    rowCover.assign(rowSum.size(), 0.0F);
    for (int y = 0; y < srcNy; ++y) {
        float* rs = rowSum.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(nxOut);
        float* rc = rowCover.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(nxOut);
        for (int i = 0; i < nxOut; ++i) {
            for (int t = tx.begin[static_cast<std::size_t>(i)]; t < tx.begin[static_cast<std::size_t>(i) + 1]; ++t) {
                const double v = value(tx.index[static_cast<std::size_t>(t)], y);
                if (std::isfinite(v)) {
                    const float w = tx.weight[static_cast<std::size_t>(t)];
                    rs[i] += w * static_cast<float>(v);
                    rc[i] += w;
                }
            }
        }
    }
    out.sum.assign(static_cast<std::size_t>(nxOut) * static_cast<std::size_t>(nyOut), 0.0F);
    out.cover.assign(out.sum.size(), 0.0F);
    for (int j = 0; j < nyOut; ++j) {
        float* os = out.sum.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(nxOut);
        float* oc = out.cover.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(nxOut);
        for (int t = ty.begin[static_cast<std::size_t>(j)]; t < ty.begin[static_cast<std::size_t>(j) + 1]; ++t) {
            const float w = ty.weight[static_cast<std::size_t>(t)];
            const std::size_t row =
                static_cast<std::size_t>(ty.index[static_cast<std::size_t>(t)]) * static_cast<std::size_t>(nxOut);
            for (int i = 0; i < nxOut; ++i) {
                os[i] += w * rowSum[row + static_cast<std::size_t>(i)];
                oc[i] += w * rowCover[row + static_cast<std::size_t>(i)];
            }
        }
    }
}

// Reduced source slices kept while the output slices that need them are made
// (each thread walks its own run of output slices in order).
class SliceCache {
public:
    template <class MakeFn>
    const ReducedSlice& get(int j, MakeFn make) {
        for (auto& e : entries_) {
            if (e.first == j) {
                return e.second;
            }
        }
        entries_.emplace_back(j, ReducedSlice{});
        make(j, entries_.back().second);
        return entries_.back().second;
    }
    void dropBelow(int j) {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [j](const auto& e) { return e.first < j; }),
                       entries_.end());
    }

private:
    std::vector<std::pair<int, ReducedSlice>> entries_;
};

}  // namespace

double RenderVolume::toQuantized(double v) const {
    const double q = (v - vmin_) / (vmax_ - vmin_) * 65535.0;
    return std::clamp(q, 0.0, 65535.0);
}

double RenderVolume::minSpacing() const {
    return std::min({sx_, sy_, sz_});
}

Vec3 RenderVolume::toPatient(const Vec3& idx) const {
    return origin_ + ax_ * (idx.x * sx_) + ay_ * (idx.y * sy_) + az_ * (idx.z * sz_);
}

Vec3 RenderVolume::toIndex(const Vec3& patient) const {
    const Vec3 rel = patient - origin_;
    return {rel.dot(ax_) / sx_, rel.dot(ay_) / sy_, rel.dot(az_) / sz_};
}

Vec3 RenderVolume::center() const {
    return toPatient({0.5 * (nx_ - 1), 0.5 * (ny_ - 1), 0.5 * (nz_ - 1)});
}

double RenderVolume::diagonalMm() const {
    const double ex = (nx_ - 1) * sx_;
    const double ey = (ny_ - 1) * sy_;
    const double ez = (nz_ - 1) * sz_;
    return std::sqrt(ex * ex + ey * ey + ez * ez);
}

float RenderVolume::sample(double x, double y, double z) const {
    if (x < 0.0 || y < 0.0 || z < 0.0 || x > nx_ - 1 || y > ny_ - 1 || z > nz_ - 1) {
        return 0.0F;
    }
    const int x0 = std::min(static_cast<int>(x), nx_ - 1);
    const int y0 = std::min(static_cast<int>(y), ny_ - 1);
    const int z0 = std::min(static_cast<int>(z), nz_ - 1);
    const int x1 = std::min(x0 + 1, nx_ - 1);
    const int y1 = std::min(y0 + 1, ny_ - 1);
    const int z1 = std::min(z0 + 1, nz_ - 1);
    const auto fx = static_cast<float>(x - x0);
    const auto fy = static_cast<float>(y - y0);
    const auto fz = static_cast<float>(z - z0);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c00 = lerp(at(x0, y0, z0), at(x1, y0, z0), fx);
    const float c10 = lerp(at(x0, y1, z0), at(x1, y1, z0), fx);
    const float c01 = lerp(at(x0, y0, z1), at(x1, y0, z1), fx);
    const float c11 = lerp(at(x0, y1, z1), at(x1, y1, z1), fx);
    return lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz);
}

RenderVolume::BuildResult RenderVolume::fromImageVolume(const ImageVolume& src, std::uint64_t maxVoxels,
                                                        const std::atomic<bool>* cancel) {
    BuildResult res;
    if (src.nx() < 2 || src.ny() < 2 || src.nz() < 2) {
        res.error = "A série tem poucos cortes para uma reconstrução 3D.";
        return res;
    }
    const int snz = src.nz();
    // Gantry tilt shifts each slice within its plane: the grid covers all of them.
    double minShX = 0.0;
    double maxShX = 0.0;
    double minShY = 0.0;
    double maxShY = 0.0;
    for (int k = 0; k < snz; ++k) {
        minShX = std::min(minShX, src.originShiftX(k));
        maxShX = std::max(maxShX, src.originShiftX(k));
        minShY = std::min(minShY, src.originShiftY(k));
        maxShY = std::max(maxShY, src.originShiftY(k));
    }
    const double extent[3] = {(src.nx() - 1 + maxShX - minShX) * src.spacingX(),
                              (src.ny() - 1 + maxShY - minShY) * src.spacingY(),
                              src.offsets().back() - src.offsets().front()};
    const double sourceSpacing[3] = {src.spacingX(), src.spacingY(), src.sliceSpacing()};
    double spacing[3];
    chooseSpacing(extent, sourceSpacing, maxVoxels, spacing);

    auto vol = std::make_shared<RenderVolume>();
    vol->nx_ = axisCount(extent[0], spacing[0]);
    vol->ny_ = axisCount(extent[1], spacing[1]);
    vol->nz_ = axisCount(extent[2], spacing[2]);
    vol->sx_ = spacing[0];
    vol->sy_ = spacing[1];
    vol->sz_ = spacing[2];
    vol->ax_ = src.rowDir();
    vol->ay_ = src.colDir();
    vol->az_ = src.normal();
    vol->origin_ =
        src.origins().front() + src.rowDir() * (minShX * src.spacingX()) + src.colDir() * (minShY * src.spacingY());
    vol->isCt_ = src.isCt();
    const double sourceVoxels = static_cast<double>(src.nx()) * src.ny() * snz;
    vol->reducedFrom_ = std::max(1.0, sourceVoxels / (static_cast<double>(vol->nx_) * vol->ny_ * vol->nz_));

    // Value range of the data (CT padding below air counts as air).
    double vmin = std::numeric_limits<double>::max();
    double vmax = std::numeric_limits<double>::lowest();
    {
        std::vector<double> mins(static_cast<std::size_t>(snz), vmin);
        std::vector<double> maxs(static_cast<std::size_t>(snz), vmax);
        parallelFor(snz, [&](int b, int e) {
            for (int k = b; k < e; ++k) {
                double lo = mins[static_cast<std::size_t>(k)];
                double hi = maxs[static_cast<std::size_t>(k)];
                for (int y = 0; y < src.ny(); ++y) {
                    for (int x = 0; x < src.nx(); ++x) {
                        const double v = src.voxel(x, y, k);
                        if (std::isfinite(v)) {
                            lo = std::min(lo, v);
                            hi = std::max(hi, v);
                        }
                    }
                }
                mins[static_cast<std::size_t>(k)] = lo;
                maxs[static_cast<std::size_t>(k)] = hi;
            }
        });
        for (int k = 0; k < snz; ++k) {
            vmin = std::min(vmin, mins[static_cast<std::size_t>(k)]);
            vmax = std::max(vmax, maxs[static_cast<std::size_t>(k)]);
        }
    }
    if (vmin > vmax) {
        res.error = "A série não contém valores válidos para a reconstrução 3D.";
        return res;
    }
    if (vol->isCt_) {
        vmin = std::max(vmin, -1024.0);
        vmax = std::max(vmax, vmin + 1.0);
    }
    if (vmax - vmin < 1e-6) {
        vmax = vmin + 1.0;
    }
    vol->vmin_ = vmin;
    vol->vmax_ = vmax;

    try {
        vol->data_.assign(static_cast<std::size_t>(vol->nx_) * static_cast<std::size_t>(vol->ny_) *
                              static_cast<std::size_t>(vol->nz_),
                          0);
    } catch (const std::bad_alloc&) {
        res.error = "Memória insuficiente para a reconstrução 3D.";
        return res;
    }

    const double rx = vol->sx_ / src.spacingX();  // source pixels per output voxel
    const double ry = vol->sy_ / src.spacingY();
    const double dz = src.sliceSpacing();
    const double scale = 65535.0 / (vmax - vmin);
    const std::vector<double>& d = src.offsets();
    const double gap = 1.5 * dz;  // as ImageVolume: farther apart is a gap of missing slices
    // Each slice stands for the space halfway to its neighbours, but never
    // more than half a slice into a gap: missing slices are not invented.
    std::vector<double> boxLo(static_cast<std::size_t>(snz));
    std::vector<double> boxHi(static_cast<std::size_t>(snz));
    for (int j = 0; j < snz; ++j) {
        const auto u = static_cast<std::size_t>(j);
        const double below = j > 0 ? d[u] - d[u - 1] : dz;
        const double above = j + 1 < snz ? d[u + 1] - d[u] : dz;
        boxLo[u] = d[u] - (below <= gap ? 0.5 * below : 0.5 * dz);
        boxHi[u] = d[u] + (above <= gap ? 0.5 * above : 0.5 * dz);
    }
    const double wz = std::max(vol->sz_, dz);
    const double fullCover = std::max(rx, 1.0) * std::max(ry, 1.0) * wz;
    const Taps ty0 = makeTaps(vol->ny_, src.ny(), ry, minShY);
    const int nxOut = vol->nx_;
    const std::size_t sliceOut = static_cast<std::size_t>(vol->nx_) * static_cast<std::size_t>(vol->ny_);

    std::atomic<bool> stopped{false};
    parallelFor(vol->nz_, [&](int kb, int ke) {
        SliceCache cache;
        std::vector<float> rowSum;
        std::vector<float> rowCover;
        std::vector<float> sum(sliceOut);
        std::vector<float> cover(sliceOut);
        auto make = [&](int j, ReducedSlice& out) {
            // Gantry tilt: each slice has its own in-plane shift.
            const Taps tx = makeTaps(nxOut, src.nx(), rx, minShX - src.originShiftX(j));
            const Taps ty =
                src.originShiftY(j) == 0.0 ? ty0 : makeTaps(vol->ny_, src.ny(), ry, minShY - src.originShiftY(j));
            reduceSlice(src.ny(), tx, ty, [&](int x, int y) { return src.voxel(x, y, j); }, out, rowSum, rowCover);
        };
        for (int k = kb; k < ke; ++k) {
            if ((cancel != nullptr && cancel->load()) || stopped.load()) {
                stopped = true;
                return;
            }
            const double c = d.front() + k * vol->sz_;
            const double lo = c - 0.5 * wz;
            const double hi = c + 0.5 * wz;
            const auto first = std::lower_bound(boxHi.begin(), boxHi.end(), lo) - boxHi.begin();
            cache.dropBelow(static_cast<int>(first));
            std::fill(sum.begin(), sum.end(), 0.0F);
            std::fill(cover.begin(), cover.end(), 0.0F);
            bool any = false;
            for (auto j = static_cast<int>(first); j < snz && boxLo[static_cast<std::size_t>(j)] < hi; ++j) {
                const double overlap =
                    std::min(hi, boxHi[static_cast<std::size_t>(j)]) - std::max(lo, boxLo[static_cast<std::size_t>(j)]);
                if (overlap <= 1e-9) {
                    continue;
                }
                const ReducedSlice& r = cache.get(j, make);
                const auto w = static_cast<float>(overlap);
                for (std::size_t p = 0; p < sliceOut; ++p) {
                    sum[p] += w * r.sum[p];
                    cover[p] += w * r.cover[p];
                }
                any = true;
            }
            if (!any) {
                continue;  // a gap of missing slices: left empty, never invented
            }
            std::uint16_t* out = vol->data_.data() + static_cast<std::size_t>(k) * sliceOut;
            for (std::size_t p = 0; p < sliceOut; ++p) {
                // Mostly outside the scan or inside a gap: empty.
                if (static_cast<double>(cover[p]) >= 0.25 * fullCover) {
                    const double q = (static_cast<double>(sum[p]) / static_cast<double>(cover[p]) - vmin) * scale;
                    out[p] = static_cast<std::uint16_t>(std::lround(std::clamp(q, 0.0, 65535.0)));
                }
            }
        }
    });
    if (stopped.load()) {
        res.error = "Cancelado.";
        return res;
    }
    vol->finalize();
    res.volume = std::move(vol);
    return res;
}

std::shared_ptr<RenderVolume> RenderVolume::fromValues(int nx, int ny, int nz, double sx, double sy, double sz,
                                                       const std::vector<float>& values, bool isCt) {
    auto vol = std::make_shared<RenderVolume>();
    vol->nx_ = nx;
    vol->ny_ = ny;
    vol->nz_ = nz;
    vol->sx_ = sx;
    vol->sy_ = sy;
    vol->sz_ = sz;
    vol->isCt_ = isCt;
    double vmin = std::numeric_limits<double>::max();
    double vmax = std::numeric_limits<double>::lowest();
    for (float v : values) {
        vmin = std::min(vmin, static_cast<double>(v));
        vmax = std::max(vmax, static_cast<double>(v));
    }
    if (values.empty()) {
        vmin = 0.0;
        vmax = 1.0;
    }
    if (vmax - vmin < 1e-6) {
        vmax = vmin + 1.0;
    }
    vol->vmin_ = vmin;
    vol->vmax_ = vmax;
    vol->data_.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        vol->data_[i] = static_cast<std::uint16_t>(std::lround(vol->toQuantized(values[i])));
    }
    vol->finalize();
    return vol;
}

std::shared_ptr<const RenderVolume> RenderVolume::reduced(const std::shared_ptr<const RenderVolume>& v, int maxDim,
                                                          std::uint64_t maxVoxels) {
    if (!v) {
        return v;
    }
    const RenderVolume& s = *v;
    const bool fitsDim = s.nx_ <= maxDim && s.ny_ <= maxDim && s.nz_ <= maxDim;
    if (fitsDim && s.voxelCount() <= maxVoxels) {
        return v;
    }
    return s.reducedCopy(maxDim, maxVoxels);
}

std::shared_ptr<const RenderVolume> RenderVolume::reducedCopy(int maxDim, std::uint64_t maxVoxels) const {
    // Same rule as the first build: thinnest axes reduced first, then the
    // dimension limit (graphics cards) on top of it.
    const double extent[3] = {(nx_ - 1) * sx_, (ny_ - 1) * sy_, (nz_ - 1) * sz_};
    const double source[3] = {sx_, sy_, sz_};
    double spacing[3];
    chooseSpacing(extent, source, maxVoxels, spacing);
    for (int a = 0; a < 3; ++a) {
        if (axisCount(extent[a], spacing[a]) > maxDim) {
            spacing[a] = extent[a] / std::max(1, maxDim - 1) * 1.0001;
        }
    }
    auto out = std::make_shared<RenderVolume>();
    out->nx_ = std::min(maxDim, axisCount(extent[0], spacing[0]));
    out->ny_ = std::min(maxDim, axisCount(extent[1], spacing[1]));
    out->nz_ = std::min(maxDim, axisCount(extent[2], spacing[2]));
    out->sx_ = spacing[0];
    out->sy_ = spacing[1];
    out->sz_ = spacing[2];
    out->ax_ = ax_;
    out->ay_ = ay_;
    out->az_ = az_;
    out->origin_ = origin_;
    out->vmin_ = vmin_;
    out->vmax_ = vmax_;
    out->isCt_ = isCt_;
    out->reducedFrom_ =
        reducedFrom_ * static_cast<double>(voxelCount()) / (static_cast<double>(out->nx_) * out->ny_ * out->nz_);
    out->data_.assign(
        static_cast<std::size_t>(out->nx_) * static_cast<std::size_t>(out->ny_) * static_cast<std::size_t>(out->nz_),
        0);
    const double fx = out->sx_ / sx_;
    const double fy = out->sy_ / sy_;
    const double fz = out->sz_ / sz_;
    const Taps tx = makeTaps(out->nx_, nx_, fx, 0.0);
    const Taps ty = makeTaps(out->ny_, ny_, fy, 0.0);
    const Taps tz = makeTaps(out->nz_, nz_, fz, 0.0);
    const std::size_t sliceOut = static_cast<std::size_t>(out->nx_) * static_cast<std::size_t>(out->ny_);
    parallelFor(out->nz_, [&](int kb, int ke) {
        SliceCache cache;
        std::vector<float> rowSum;
        std::vector<float> rowCover;
        std::vector<float> sum(sliceOut);
        std::vector<float> cover(sliceOut);
        auto make = [&](int j, ReducedSlice& r) {
            reduceSlice(
                ny_, tx, ty, [&](int x, int y) { return static_cast<double>(at(x, y, j)); }, r, rowSum, rowCover);
        };
        for (int k = kb; k < ke; ++k) {
            const int t0 = tz.begin[static_cast<std::size_t>(k)];
            const int t1 = tz.begin[static_cast<std::size_t>(k) + 1];
            if (t0 < t1) {
                cache.dropBelow(tz.index[static_cast<std::size_t>(t0)]);
            }
            std::fill(sum.begin(), sum.end(), 0.0F);
            std::fill(cover.begin(), cover.end(), 0.0F);
            for (int t = t0; t < t1; ++t) {
                const ReducedSlice& r = cache.get(tz.index[static_cast<std::size_t>(t)], make);
                const float w = tz.weight[static_cast<std::size_t>(t)];
                for (std::size_t p = 0; p < sliceOut; ++p) {
                    sum[p] += w * r.sum[p];
                    cover[p] += w * r.cover[p];
                }
            }
            std::uint16_t* dst = out->data_.data() + static_cast<std::size_t>(k) * sliceOut;
            for (std::size_t p = 0; p < sliceOut; ++p) {
                if (cover[p] > 0.0F) {
                    dst[p] = static_cast<std::uint16_t>(
                        std::lround(std::clamp(static_cast<double>(sum[p] / cover[p]), 0.0, 65535.0)));
                }
            }
        }
    });
    out->finalize();
    return out;
}

double RenderVolume::percentile(double p) const {
    std::uint64_t total = 0;
    for (auto c : histogram_) {
        total += c;
    }
    if (total == 0) {
        return vmin_;
    }
    const double target = std::clamp(p, 0.0, 1.0) * static_cast<double>(total);
    std::uint64_t acc = 0;
    for (int b = 0; b < kHistogramBins; ++b) {
        acc += histogram_[static_cast<std::size_t>(b)];
        if (static_cast<double>(acc) >= target) {
            return toValue((b + 0.5) * 65536.0 / kHistogramBins);
        }
    }
    return vmax_;
}

void RenderVolume::finalize() {
    bx_ = (nx_ + kBlock - 1) / kBlock;
    by_ = (ny_ + kBlock - 1) / kBlock;
    bz_ = (nz_ + kBlock - 1) / kBlock;
    const std::size_t blocks =
        static_cast<std::size_t>(bx_) * static_cast<std::size_t>(by_) * static_cast<std::size_t>(bz_);
    blockMin_.assign(blocks, 65535);
    blockMax_.assign(blocks, 0);
    histogram_.assign(kHistogramBins, 0);
    std::vector<std::vector<std::uint32_t>> partial(static_cast<std::size_t>(bz_));
    parallelFor(bz_, [&](int b, int e) {
        for (int bk = b; bk < e; ++bk) {
            auto& hist = partial[static_cast<std::size_t>(bk)];
            hist.assign(kHistogramBins, 0);
            const int z0 = bk * kBlock;
            const int z1 = std::min(nz_ - 1, z0 + kBlock);
            for (int bj = 0; bj < by_; ++bj) {
                const int y0 = bj * kBlock;
                const int y1 = std::min(ny_ - 1, y0 + kBlock);
                for (int bi = 0; bi < bx_; ++bi) {
                    const int x0 = bi * kBlock;
                    const int x1 = std::min(nx_ - 1, x0 + kBlock);
                    std::uint16_t lo = 65535;
                    std::uint16_t hi = 0;
                    for (int z = z0; z <= z1; ++z) {
                        for (int y = y0; y <= y1; ++y) {
                            for (int x = x0; x <= x1; ++x) {
                                const std::uint16_t v = at(x, y, z);
                                lo = std::min(lo, v);
                                hi = std::max(hi, v);
                                // Histogram: each voxel once (its own block only).
                                if (v != 0 && z < z0 + kBlock && y < y0 + kBlock && x < x0 + kBlock) {
                                    ++hist[static_cast<std::size_t>(v >> 4)];
                                }
                            }
                        }
                    }
                    const std::size_t idx =
                        (static_cast<std::size_t>(bk) * static_cast<std::size_t>(by_) + static_cast<std::size_t>(bj)) *
                            static_cast<std::size_t>(bx_) +
                        static_cast<std::size_t>(bi);
                    blockMin_[idx] = lo;
                    blockMax_[idx] = hi;
                }
            }
        }
    });
    for (const auto& hist : partial) {
        for (int i = 0; i < kHistogramBins && !hist.empty(); ++i) {
            histogram_[static_cast<std::size_t>(i)] += hist[static_cast<std::size_t>(i)];
        }
    }
}

}  // namespace vtc
