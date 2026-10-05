#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <random>

#include "dicom/DicomGeometry.h"
#include "mpr/ImageVolume.h"
#include "mpr/MprGeometry.h"
#include "mpr/Reslicer.h"

using namespace vtc;

namespace {

// A linear field is reproduced exactly by (tri)linear interpolation, so any
// geometric error in the reslicer shows up as a numeric mismatch.
double field(const Vec3& p) { return 3.0 + 2.0 * p.x - 1.5 * p.y + 0.75 * p.z; }

struct Phantom {
    int nx = 40, ny = 30;
    double sx = 0.8, sy = 0.6;
    Vec3 row{1, 0, 0}, col{0, 1, 0};
    std::vector<Vec3> origins;
};

std::shared_ptr<ImageVolume> buildPhantom(const Phantom& ph) {
    std::vector<std::vector<float>> slices;
    for (const auto& o : ph.origins) {
        std::vector<float> s(static_cast<size_t>(ph.nx * ph.ny));
        for (int y = 0; y < ph.ny; ++y) {
            for (int x = 0; x < ph.nx; ++x) {
                const Vec3 p = o + ph.row * (x * ph.sx) + ph.col * (y * ph.sy);
                s[static_cast<size_t>(y * ph.nx + x)] = static_cast<float>(field(p));
            }
        }
        slices.push_back(std::move(s));
    }
    return ImageVolume::fromSlices(ph.nx, ph.ny, ph.row, ph.col, ph.sx, ph.sy, ph.origins, slices);
}

// Random points strictly inside the (possibly sheared) volume.
std::vector<Vec3> interiorPoints(const Phantom& ph, int count, unsigned seed) {
    std::mt19937 gen(seed);
    std::uniform_real_distribution<double> ux(1.0, ph.nx - 2.0);
    std::uniform_real_distribution<double> uy(1.0, ph.ny - 2.0);
    std::uniform_real_distribution<double> uk(0.0, static_cast<double>(ph.origins.size() - 1));
    std::vector<Vec3> pts;
    for (int i = 0; i < count; ++i) {
        const double kf = uk(gen);
        const auto k0 = static_cast<size_t>(std::floor(kf));
        const auto k1 = std::min(k0 + 1, ph.origins.size() - 1);
        const double t = kf - static_cast<double>(k0);
        const Vec3 o = ph.origins[k0] * (1.0 - t) + ph.origins[k1] * t;
        pts.push_back(o + ph.row * (ux(gen) * ph.sx) + ph.col * (uy(gen) * ph.sy));
    }
    return pts;
}

}  // namespace

TEST_CASE("Volume sampling is exact for a linear phantom (axial)", "[mpr]") {
    Phantom ph;
    for (int k = 0; k < 25; ++k) {
        ph.origins.push_back({-10.0, -5.0, k * 2.0});
    }
    const auto vol = buildPhantom(ph);
    REQUIRE(vol->uniform());
    REQUIRE(vol->sliceSpacing() == Catch::Approx(2.0));
    for (const auto& p : interiorPoints(ph, 500, 1)) {
        REQUIRE(vol->sample(p, Interpolation::Linear) == Catch::Approx(field(p)).margin(1e-3));
    }
    // outside -> NaN
    REQUIRE(std::isnan(vol->sample({-100, 0, 10}, Interpolation::Linear)));
    REQUIRE(std::isnan(vol->sample({0, 0, 60}, Interpolation::Linear)));
}

TEST_CASE("Gantry tilt (sheared origins) is sampled exactly", "[mpr]") {
    Phantom ph;
    for (int k = 0; k < 20; ++k) {
        ph.origins.push_back({-10.0, -5.0 + k * 0.6, k * 2.5});
    }
    const auto vol = buildPhantom(ph);
    for (const auto& p : interiorPoints(ph, 500, 2)) {
        REQUIRE(vol->sample(p, Interpolation::Linear) == Catch::Approx(field(p)).margin(1e-3));
    }
}

TEST_CASE("Irregular slice spacing uses the real positions", "[mpr]") {
    Phantom ph;
    const std::vector<double> steps{1.0, 1.25, 0.9, 1.1, 1.0, 1.3, 0.8, 1.0, 1.2, 1.0};
    double z = 0.0;
    ph.origins.push_back({0, 0, z});
    for (double s : steps) {
        z += s;
        ph.origins.push_back({0, 0, z});
    }
    const auto vol = buildPhantom(ph);
    REQUIRE_FALSE(vol->uniform());
    for (const auto& p : interiorPoints(ph, 500, 3)) {
        REQUIRE(vol->sample(p, Interpolation::Linear) == Catch::Approx(field(p)).margin(1e-3));
    }
}

TEST_CASE("Gaps of missing slices are left empty, never interpolated", "[mpr]") {
    Phantom ph;
    for (double zz : {0.0, 1.0, 2.0, 3.0, 7.0, 8.0, 9.0}) {
        ph.origins.push_back({0, 0, zz});
    }
    const auto vol = buildPhantom(ph);
    REQUIRE(std::isnan(vol->sample({5, 5, 5.0}, Interpolation::Linear)));
    REQUIRE(vol->sample({5, 5, 1.5}, Interpolation::Linear) == Catch::Approx(field({5, 5, 1.5})).margin(1e-3));
    REQUIRE(vol->sample({5, 5, 7.5}, Interpolation::Linear) == Catch::Approx(field({5, 5, 7.5})).margin(1e-3));
}

TEST_CASE("Oblique acquisition volume", "[mpr]") {
    Phantom ph;
    const double a = 0.35;
    ph.row = {std::cos(a), std::sin(a), 0};
    ph.col = {0, 0, -1};
    const Vec3 n = ph.row.cross(ph.col).normalized();
    for (int k = 0; k < 15; ++k) {
        ph.origins.push_back(Vec3{20, -30, 40} + n * (k * 1.5));
    }
    const auto vol = buildPhantom(ph);
    for (const auto& p : interiorPoints(ph, 400, 4)) {
        REQUIRE(vol->sample(p, Interpolation::Linear) == Catch::Approx(field(p)).margin(1e-3));
    }
}

TEST_CASE("Orthogonal MPR views use anatomical screen conventions", "[mpr]") {
    Phantom ph;
    for (int k = 0; k < 25; ++k) {
        ph.origins.push_back({-10.0, -5.0, k * 2.0});
    }
    const auto vol = buildPhantom(ph);
    const auto ax = makeOrthogonalView(*vol, MprOrientation::Axial);
    const auto co = makeOrthogonalView(*vol, MprOrientation::Coronal);
    const auto sa = makeOrthogonalView(*vol, MprOrientation::Sagittal);
    REQUIRE(ax.u.x == Catch::Approx(1.0));
    REQUIRE(ax.v.y == Catch::Approx(1.0));
    REQUIRE(co.u.x == Catch::Approx(1.0));
    REQUIRE(co.v.z == Catch::Approx(-1.0));
    REQUIRE(sa.u.y == Catch::Approx(1.0));
    REQUIRE(sa.v.z == Catch::Approx(-1.0));
    REQUIRE(ax.step == Catch::Approx(2.0));  // native slice spacing
    REQUIRE(co.step == Catch::Approx(0.6));  // row spacing
    REQUIRE(sa.step == Catch::Approx(0.8));  // column spacing
    REQUIRE(ax.sliceCount == 25);
    REQUIRE(co.height == 25);
    REQUIRE(co.spacingV == Catch::Approx(2.0));

    const Vec3 c = vol->center();
    for (const auto* view : {&ax, &co, &sa}) {
        const auto plane = view->planeThrough(c);
        const auto img = reslice(*vol, plane, {}, Interpolation::Linear);
        REQUIRE(img->width == view->width);
        int checked = 0;
        for (int j = 1; j < img->height - 1; j += 3) {
            for (int i = 1; i < img->width - 1; i += 3) {
                const Vec3 p = plane.pixelToPatient(i, j);
                const double v = img->rawAt(i, j);
                REQUIRE_FALSE(std::isnan(v));
                REQUIRE(v == Catch::Approx(field(p)).margin(1e-3));
                ++checked;
            }
        }
        REQUIRE(checked > 20);
    }
}

TEST_CASE("Native plane MPR reproduces the original voxels", "[mpr]") {
    Phantom ph;
    for (int k = 0; k < 10; ++k) {
        ph.origins.push_back({-10.0, -5.0, k * 2.0});
    }
    const auto vol = buildPhantom(ph);
    const auto ax = makeOrthogonalView(*vol, MprOrientation::Axial);
    const Vec3 at = ax.pointAtSlice(vol->center(), 4);
    const auto img = reslice(*vol, ax.planeThrough(at), {}, Interpolation::Nearest);
    for (int j = 0; j < ph.ny; j += 4) {
        for (int i = 0; i < ph.nx; i += 4) {
            REQUIRE(img->rawAt(i, j) == Catch::Approx(vol->voxel(i, j, 4)).margin(1e-4));
        }
    }
}

TEST_CASE("Thick slab: average, MIP and MinIP", "[mpr]") {
    Phantom ph;
    for (int k = 0; k < 40; ++k) {
        ph.origins.push_back({-10.0, -5.0, k * 1.0});
    }
    const auto vol = buildPhantom(ph);
    const auto ax = makeOrthogonalView(*vol, MprOrientation::Axial);
    const Vec3 c{5.0, 4.0, 20.0};
    const auto plane = ax.planeThrough(c);
    // pixel nearest to c
    const int i = static_cast<int>(std::lround((c - plane.origin).dot(plane.u) / plane.spacingU));
    const int j = static_cast<int>(std::lround((c - plane.origin).dot(plane.v) / plane.spacingV));
    const Vec3 p = plane.pixelToPatient(i, j);
    const double t = 10.0;
    const auto avg = reslice(*vol, plane, {t, SlabMode::Average}, Interpolation::Linear);
    const auto mip = reslice(*vol, plane, {t, SlabMode::MIP}, Interpolation::Linear);
    const auto minip = reslice(*vol, plane, {t, SlabMode::MinIP}, Interpolation::Linear);
    REQUIRE(avg->rawAt(i, j) == Catch::Approx(field(p)).margin(1e-3));
    REQUIRE(mip->rawAt(i, j) == Catch::Approx(field(p + plane.normal() * (t / 2))).margin(1e-3));
    REQUIRE(minip->rawAt(i, j) == Catch::Approx(field(p - plane.normal() * (t / 2))).margin(1e-3));
}

TEST_CASE("Volume built from decoded int16 frames keeps HU", "[mpr]") {
    // Three frames 4x4, stored = HU + 1024
    auto inst = std::make_shared<InstanceInfo>();
    inst->rows = 4;
    inst->columns = 4;
    inst->samplesPerPixel = 1;
    inst->bitsAllocated = 16;
    inst->bitsStored = 12;
    inst->pixelRepresentation = 0;
    inst->numberOfFrames = 3;
    inst->modality = "CT";
    inst->hasPixelData = true;
    for (int k = 0; k < 3; ++k) {
        FrameInfo fi;
        fi.frameIndex = k;
        fi.geometry.hasPosition = fi.geometry.hasOrientation = true;
        fi.geometry.position = {0, 0, k * 3.0};
        fi.geometry.spacingSource = SpacingSource::PixelSpacing;
        fi.rescaleIntercept = -1024.0;
        inst->frames.push_back(fi);
    }
    Series s;
    s.id = "x";
    for (int k = 2; k >= 0; --k) {
        s.frames.push_back({inst, k});  // reverse display order
    }
    s.geometry = analyzeStack(s.frames);
    REQUIRE(s.geometry.volumetric);
    auto fetch = [](const FrameRef& r) -> DecodedFramePtr {
        auto f = std::make_shared<DecodedFrame>();
        f->width = f->height = 4;
        f->format = PixelFormat::U16;
        f->data.resize(32);
        auto* p = f->as<std::uint16_t>();
        for (int i = 0; i < 16; ++i) {
            p[i] = static_cast<std::uint16_t>(1024 + 100 * r.frame + i);
        }
        f->intercept = -1024.0;
        return f;
    };
    const auto res = ImageVolume::build(s, fetch, 1u << 20);
    REQUIRE(res.volume);
    REQUIRE(res.volume->voxel(1, 0, 0) == Catch::Approx(1.0));     // slice z=0 -> frame 0
    REQUIRE(res.volume->voxel(1, 0, 2) == Catch::Approx(201.0));   // ascending order
    REQUIRE(res.volume->byteSize() == 3 * 16 * 2);                  // int16 storage
    const auto tooBig = ImageVolume::build(s, fetch, 10);
    REQUIRE_FALSE(tooBig.volume);
    REQUIRE_FALSE(tooBig.error.empty());
}
