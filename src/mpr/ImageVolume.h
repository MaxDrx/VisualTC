#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Vec3.h"
#include "dicom/DicomSeries.h"
#include "imaging/PixelData.h"

namespace vtc {

enum class Interpolation { Nearest, Linear };

// A stack of parallel slices in patient space. Slices are stored in
// ascending order along `normal`, each with its own origin, so irregular
// spacing and gantry tilt (sheared origins) are represented exactly instead
// of being forced onto a regular grid.
class ImageVolume {
public:
    using FrameFetcher = std::function<DecodedFramePtr(const FrameRef&)>;
    using Progress = std::function<void(int done, int total)>;

    struct BuildResult {
        std::shared_ptr<ImageVolume> volume;
        std::string error;  // pt-BR
    };

    // Builds from a volumetric series. `fetch` returns the decoded frame
    // (from cache or by decoding); it is called from the calling thread.
    static BuildResult build(const Series& series, const FrameFetcher& fetch, std::uint64_t maxBytes,
                             const Progress& progress = {}, const std::atomic<bool>* cancel = nullptr);

    // Builds from raw float slices (used by tests and synthetic data).
    static std::shared_ptr<ImageVolume> fromSlices(int nx, int ny, const Vec3& rowDir, const Vec3& colDir, double sx,
                                                   double sy, const std::vector<Vec3>& origins,
                                                   const std::vector<std::vector<float>>& slices);

    [[nodiscard]] int nx() const { return nx_; }
    [[nodiscard]] int ny() const { return ny_; }
    [[nodiscard]] int nz() const { return nz_; }
    [[nodiscard]] const Vec3& rowDir() const { return rowDir_; }
    [[nodiscard]] const Vec3& colDir() const { return colDir_; }
    [[nodiscard]] const Vec3& normal() const { return normal_; }
    [[nodiscard]] double spacingX() const { return sx_; }
    [[nodiscard]] double spacingY() const { return sy_; }
    [[nodiscard]] double sliceSpacing() const { return dz_; }  // median
    [[nodiscard]] const std::vector<Vec3>& origins() const { return origins_; }
    [[nodiscard]] const std::vector<double>& offsets() const { return d_; }
    [[nodiscard]] bool uniform() const { return uniform_; }
    [[nodiscard]] std::uint64_t byteSize() const;
    [[nodiscard]] std::string seriesId() const { return seriesId_; }
    [[nodiscard]] std::string frameOfReferenceUid() const { return frameOfReference_; }
    [[nodiscard]] bool isCt() const { return isCt_; }

    // The 8 corners of the volume (centres of the corner voxels).
    [[nodiscard]] std::vector<Vec3> corners() const;
    [[nodiscard]] Vec3 center() const;

    // Modality value at patient point p, NaN outside the volume or inside a
    // gap between non-contiguous slices (missing slices are never invented).
    [[nodiscard]] double sample(const Vec3& p, Interpolation interp) const;

    // Raw voxel access (modality value).
    [[nodiscard]] double voxel(int x, int y, int z) const;

    // Linear-form helpers used by the reslicer.
    [[nodiscard]] bool locateSlice(double dn, int& k0, int& k1, double& t) const;
    [[nodiscard]] double sliceSample(int k, double x, double y, Interpolation interp) const;
    [[nodiscard]] double originShiftX(int k) const { return shiftX_[static_cast<size_t>(k)]; }
    [[nodiscard]] double originShiftY(int k) const { return shiftY_[static_cast<size_t>(k)]; }

private:
    void finalizeGeometry();

    int nx_ = 0;
    int ny_ = 0;
    int nz_ = 0;
    Vec3 rowDir_{1, 0, 0};
    Vec3 colDir_{0, 1, 0};
    Vec3 normal_{0, 0, 1};
    double sx_ = 1.0;
    double sy_ = 1.0;
    double dz_ = 1.0;
    double gapThreshold_ = 1.5;
    bool uniform_ = true;
    std::vector<Vec3> origins_;
    std::vector<double> d_;
    std::vector<double> shiftX_;  // in-plane shift of each slice origin vs slice 0 (pixels)
    std::vector<double> shiftY_;
    bool isFloat_ = false;
    double slope_ = 1.0;
    double intercept_ = 0.0;
    std::vector<std::int16_t> i16_;
    std::vector<float> f32_;
    std::string seriesId_;
    std::string frameOfReference_;
    bool isCt_ = false;
};

using VolumePtr = std::shared_ptr<const ImageVolume>;

}  // namespace vtc
