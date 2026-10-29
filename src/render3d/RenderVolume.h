#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Vec3.h"
#include "mpr/ImageVolume.h"

namespace vtc {

// Regular voxel grid used by the 3D renderers (CPU and GPU).
//
// The slices of the ImageVolume are resampled at a uniform spacing along the
// normal, so irregular spacing and gantry tilt are resolved here and the
// renderers can treat the data as a plain 3D array. Values are quantized to
// 16 bits over [minValue, maxValue]; gaps of missing slices and the space
// outside the scan hold quantized 0 (the minimum, transparent in every
// preset). The grid can be reduced (box filter) to fit a voxel budget: RAM
// on old computers, texture memory on graphics cards.
class RenderVolume {
public:
    static constexpr int kBlock = 8;  // empty-space skipping blocks (voxels per side)

    struct BuildResult {
        std::shared_ptr<RenderVolume> volume;
        std::string error;  // pt-BR, translated by the interface
    };

    static BuildResult fromImageVolume(const ImageVolume& src, std::uint64_t maxVoxels,
                                       const std::atomic<bool>* cancel = nullptr);
    // Test/synthetic data: values in x-fastest order, axes along patient x, y, z.
    static std::shared_ptr<RenderVolume> fromValues(int nx, int ny, int nz, double sx, double sy, double sz,
                                                    const std::vector<float>& values, bool isCt);
    // Smaller copy with every dimension <= maxDim and at most maxVoxels voxels
    // (the same volume when it already fits).
    [[nodiscard]] static std::shared_ptr<const RenderVolume> reduced(const std::shared_ptr<const RenderVolume>& v,
                                                                     int maxDim, std::uint64_t maxVoxels);
    // Editable copy (3D scissors); call refresh() after changing the voxels.
    [[nodiscard]] std::shared_ptr<RenderVolume> clone() const { return std::make_shared<RenderVolume>(*this); }
    [[nodiscard]] std::uint16_t* mutableData() { return data_.data(); }
    void refresh() { finalize(); }

    [[nodiscard]] int nx() const { return nx_; }
    [[nodiscard]] int ny() const { return ny_; }
    [[nodiscard]] int nz() const { return nz_; }
    [[nodiscard]] double spacingX() const { return sx_; }
    [[nodiscard]] double spacingY() const { return sy_; }
    [[nodiscard]] double spacingZ() const { return sz_; }
    [[nodiscard]] double minSpacing() const;
    [[nodiscard]] const Vec3& origin() const { return origin_; }  // centre of voxel (0,0,0)
    [[nodiscard]] const Vec3& axisX() const { return ax_; }
    [[nodiscard]] const Vec3& axisY() const { return ay_; }
    [[nodiscard]] const Vec3& axisZ() const { return az_; }
    [[nodiscard]] double minValue() const { return vmin_; }
    [[nodiscard]] double maxValue() const { return vmax_; }
    [[nodiscard]] bool isCt() const { return isCt_; }
    [[nodiscard]] const std::uint16_t* data() const { return data_.data(); }
    [[nodiscard]] std::uint64_t voxelCount() const { return data_.size(); }
    [[nodiscard]] bool reducedFromSource() const { return reducedFrom_ > 1.0; }
    [[nodiscard]] double reductionFactor() const { return reducedFrom_; }

    [[nodiscard]] std::uint16_t at(int x, int y, int z) const {
        return data_[(static_cast<std::size_t>(z) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(y)) *
                         static_cast<std::size_t>(nx_) +
                     static_cast<std::size_t>(x)];
    }
    // Quantized value (0..65535) <-> modality value (HU for CT).
    [[nodiscard]] double toValue(double q) const { return vmin_ + q * (vmax_ - vmin_) / 65535.0; }
    [[nodiscard]] double toQuantized(double v) const;
    // Trilinear sample in index coordinates (0 outside), quantized units.
    [[nodiscard]] float sample(double x, double y, double z) const;

    // Patient <-> index coordinates.
    [[nodiscard]] Vec3 toPatient(const Vec3& idx) const;
    [[nodiscard]] Vec3 toIndex(const Vec3& patient) const;
    [[nodiscard]] Vec3 center() const;
    [[nodiscard]] double diagonalMm() const;

    // Min/max quantized value of each kBlock³ block, including the next
    // voxel on each axis so that interpolated samples stay within bounds.
    [[nodiscard]] int blocksX() const { return bx_; }
    [[nodiscard]] int blocksY() const { return by_; }
    [[nodiscard]] int blocksZ() const { return bz_; }
    [[nodiscard]] std::uint16_t blockMin(int i) const { return blockMin_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] std::uint16_t blockMax(int i) const { return blockMax_[static_cast<std::size_t>(i)]; }

    // Value (modality units) below which a fraction p of the voxels inside
    // the scanned region lies (MR presets adapt to the intensity range).
    [[nodiscard]] double percentile(double p) const;
    // Voxel count per 1/4096 of the quantized range (empty voxels excluded).
    [[nodiscard]] const std::vector<std::uint32_t>& histogram() const { return histogram_; }

private:
    void finalize();
    [[nodiscard]] std::shared_ptr<const RenderVolume> reducedCopy(int maxDim, std::uint64_t maxVoxels) const;

    int nx_ = 0, ny_ = 0, nz_ = 0;
    double sx_ = 1, sy_ = 1, sz_ = 1;
    Vec3 origin_;
    Vec3 ax_{1, 0, 0}, ay_{0, 1, 0}, az_{0, 0, 1};
    double vmin_ = 0, vmax_ = 1;
    bool isCt_ = false;
    double reducedFrom_ = 1.0;
    std::vector<std::uint16_t> data_;
    int bx_ = 0, by_ = 0, bz_ = 0;
    std::vector<std::uint16_t> blockMin_, blockMax_;
    std::vector<std::uint32_t> histogram_;  // 4096 bins over the quantized range, excluding 0
};

using RenderVolumePtr = std::shared_ptr<const RenderVolume>;

}  // namespace vtc
