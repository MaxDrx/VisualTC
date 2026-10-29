// 3D reconstruction core: resampling, transfer tables, camera, CPU ray
// casting (MIP, volume, surface, crop box, empty-space skipping) and the
// 3D scissors.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "mpr/ImageVolume.h"
#include "render3d/Camera3D.h"
#include "render3d/CpuRayCaster.h"
#include "render3d/RenderSettings.h"
#include "render3d/RenderVolume.h"
#include "render3d/Sculpt.h"
#include "render3d/TransferFunction.h"

using namespace vtc;
using Catch::Approx;

namespace {

// n^3 voxels of 1 mm with a ball of `inside` (radius r around the centre)
// over `outside`.
std::shared_ptr<RenderVolume> ball(int n, double r, float inside, float outside) {
    std::vector<float> values(static_cast<std::size_t>(n) * n * n);
    const double c = (n - 1) / 2.0;
    for (int z = 0; z < n; ++z) {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const double d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c) + (z - c) * (z - c));
                values[static_cast<std::size_t>((z * n + y) * n + x)] = d <= r ? inside : outside;
            }
        }
    }
    return RenderVolume::fromValues(n, n, n, 1.0, 1.0, 1.0, values, true);
}

TransferFunction boneLike() {
    TransferFunction tf;
    tf.points = {{-1000.0, 0, 0, 0, 0}, {100.0, 1, 1, 1, 0}, {300.0, 1, 1, 1, 0.9F}, {3000.0, 1, 1, 1, 0.9F}};
    return tf;
}

std::vector<std::uint32_t> render(const RenderVolume& v, const TransferFunction& tf, const Camera3D& cam,
                                  const RenderSettings& s, int size) {
    std::vector<std::uint32_t> img(static_cast<std::size_t>(size) * size);
    const TransferLut lut = buildTransferLut(tf, v, s.stepMm(v.minSpacing()));
    REQUIRE(CpuRayCaster::render(v, lut, cam, s, size, size, img.data()));
    return img;
}

int gray(std::uint32_t p) {
    return static_cast<int>((p >> 8) & 0xFF);
}

}  // namespace

TEST_CASE("Volume 3D: reamostragem regular a partir dos cortes", "[render3d]") {
    // 6 slices 2 mm apart, value = 10 * z(mm) + x.
    std::vector<Vec3> origins;
    std::vector<std::vector<float>> slices;
    for (int k = 0; k < 6; ++k) {
        origins.push_back({0, 0, 2.0 * k});
        std::vector<float> s(8 * 8);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                s[static_cast<std::size_t>(y * 8 + x)] = static_cast<float>(10.0 * 2.0 * k + x);
            }
        }
        slices.push_back(s);
    }
    const auto src = ImageVolume::fromSlices(8, 8, {1, 0, 0}, {0, 1, 0}, 1.0, 1.0, origins, slices);
    const auto built = RenderVolume::fromImageVolume(*src, 1ULL << 30);
    REQUIRE(built.volume);
    const RenderVolume& v = *built.volume;
    REQUIRE(v.nx() == 8);
    REQUIRE(v.ny() == 8);
    REQUIRE(v.nz() == 6);
    REQUIRE(v.spacingZ() == Approx(2.0));
    REQUIRE_FALSE(v.reducedFromSource());
    // Values come back (quantized to 16 bits over the range).
    REQUIRE(v.toValue(v.at(3, 2, 4)) == Approx(10.0 * 8.0 + 3.0).margin(0.05));
    REQUIRE(v.toValue(v.toQuantized(57.0)) == Approx(57.0).margin(0.01));
    // Index <-> patient.
    const Vec3 p = v.toPatient({3, 2, 4});
    REQUIRE(p.z == Approx(8.0));
    const Vec3 back = v.toIndex(p);
    REQUIRE(back.x == Approx(3.0));
    REQUIRE(back.z == Approx(4.0));
}

TEST_CASE("Volume 3D: inclinação do gantry e cortes faltantes", "[render3d]") {
    std::vector<Vec3> origins;
    std::vector<std::vector<float>> slices;
    // Each slice shifted 1 mm in x (tilt); slices 3 and 4 missing (gap of 3 mm).
    // Air in the first two columns, 500 elsewhere.
    for (int k : {0, 1, 2, 5, 6, 7}) {
        origins.push_back({1.0 * k, 0, 1.0 * k});
        std::vector<float> s(16 * 16, 500.0F);
        for (int y = 0; y < 16; ++y) {
            s[static_cast<std::size_t>(y * 16)] = -1000.0F;
            s[static_cast<std::size_t>(y * 16 + 1)] = -1000.0F;
        }
        slices.push_back(s);
    }
    const auto src = ImageVolume::fromSlices(16, 16, {1, 0, 0}, {0, 1, 0}, 1.0, 1.0, origins, slices);
    const auto built = RenderVolume::fromImageVolume(*src, 1ULL << 30);
    REQUIRE(built.volume);
    const RenderVolume& v = *built.volume;
    REQUIRE(v.nx() == 16 + 7);  // covers every shifted slice
    REQUIRE(v.nz() == 8);
    // Slice 0 lies at x 0..15, slice 7 at x 7..22 (patient mm).
    REQUIRE(v.toValue(v.at(5, 5, 0)) == Approx(500.0).margin(0.5));
    REQUIRE(v.at(22, 5, 0) == 0);
    REQUIRE(v.toValue(v.at(22, 5, 7)) == Approx(500.0).margin(0.5));
    REQUIRE(v.at(7, 5, 7) == 0);  // the air of slice 7 is where its own columns are
    // The gap stays empty: nothing invented between slices 2 and 5.
    REQUIRE(v.at(8, 5, 3) == 0);
    REQUIRE(v.at(8, 5, 4) == 0);
}

TEST_CASE("Volume 3D: redução respeita o limite de voxels", "[render3d]") {
    std::vector<Vec3> origins;
    std::vector<std::vector<float>> slices;
    for (int k = 0; k < 40; ++k) {
        origins.push_back({0, 0, 0.5 * k});
        slices.emplace_back(64 * 64, static_cast<float>(k));
    }
    const auto src = ImageVolume::fromSlices(64, 64, {1, 0, 0}, {0, 1, 0}, 0.5, 0.5, origins, slices);
    const auto built = RenderVolume::fromImageVolume(*src, 20000);
    REQUIRE(built.volume);
    const RenderVolume& v = *built.volume;
    REQUIRE(v.voxelCount() <= 20000);
    REQUIRE(v.reducedFromSource());
    // The physical size is kept.
    REQUIRE((v.nx() - 1) * v.spacingX() == Approx(31.5).margin(v.spacingX()));
    REQUIRE((v.nz() - 1) * v.spacingZ() == Approx(19.5).margin(v.spacingZ()));
    // Reduction of an existing volume, also under a size limit per axis.
    const auto shared = std::shared_ptr<const RenderVolume>(built.volume);
    REQUIRE(RenderVolume::reduced(shared, 4096, 1ULL << 30) == shared);  // already fits: the same
    const auto small = RenderVolume::reduced(shared, 8, 1ULL << 30);
    REQUIRE(small->nx() <= 8);
    REQUIRE(small->ny() <= 8);
    REQUIRE(small->nz() <= 8);
    REQUIRE(small->minValue() == Approx(v.minValue()));
}

TEST_CASE("Volume 3D: mínimos e máximos dos blocos", "[render3d]") {
    const auto v = ball(20, 6.0, 1000.0F, -1000.0F);
    for (int bk = 0; bk < v->blocksZ(); ++bk) {
        for (int bj = 0; bj < v->blocksY(); ++bj) {
            for (int bi = 0; bi < v->blocksX(); ++bi) {
                std::uint16_t lo = 65535;
                std::uint16_t hi = 0;
                for (int z = bk * 8; z <= std::min(v->nz() - 1, bk * 8 + 8); ++z) {
                    for (int y = bj * 8; y <= std::min(v->ny() - 1, bj * 8 + 8); ++y) {
                        for (int x = bi * 8; x <= std::min(v->nx() - 1, bi * 8 + 8); ++x) {
                            lo = std::min(lo, v->at(x, y, z));
                            hi = std::max(hi, v->at(x, y, z));
                        }
                    }
                }
                const int b = (bk * v->blocksY() + bj) * v->blocksX() + bi;
                REQUIRE(v->blockMin(b) == lo);
                REQUIRE(v->blockMax(b) == hi);
            }
        }
    }
}

TEST_CASE("Tabela de transferência: opacidade corrigida pela distância", "[render3d]") {
    const auto v = ball(8, 3.0, 1000.0F, -1000.0F);
    TransferFunction tf;
    tf.points = {{-1000.0, 0, 0, 0, 0}, {0.0, 1, 0, 0, 0}, {500.0, 1, 0, 0, 0.5F}, {1000.0, 1, 1, 1, 0.5F}};
    const TransferLut one = buildTransferLut(tf, *v, kOpacityReferenceMm);
    const TransferLut two = buildTransferLut(tf, *v, 2.0 * kOpacityReferenceMm);
    const int last = TransferLut::kSize - 1;
    REQUIRE(one.rgba[static_cast<std::size_t>(last) * 4 + 3] == Approx(0.5));
    REQUIRE(two.rgba[static_cast<std::size_t>(last) * 4 + 3] == Approx(0.75));  // 1 - (1 - 0.5)^2
    REQUIRE(one.rgba[3] == 0.0F);                                               // the minimum is always clear
    // anyOpaque agrees with the table itself.
    const int q0 = static_cast<int>(v->toQuantized(-1000.0));
    const int qMid = static_cast<int>(v->toQuantized(-600.0));
    REQUIRE_FALSE(one.anyOpaque(q0, std::max(q0, qMid - 2000)));
    REQUIRE(one.anyOpaque(q0, 65535));
    // A window drag moves the whole function.
    const TransferFunction moved = tf.transformed(100.0, 1.0, 0.0);
    REQUIRE(moved.points[1].value == Approx(100.0));
    REQUIRE(moved.evaluate(600.0)[3] == Approx(0.5));
}

TEST_CASE("Predefinições 3D resolvem para qualquer volume", "[render3d]") {
    const auto ct = ball(16, 5.0, 700.0F, -1000.0F);
    const auto mr = RenderVolume::fromValues(16, 16, 16, 1, 1, 1, std::vector<float>(4096, 300.0F), false);
    REQUIRE(renderPresets().size() >= 10);
    for (const auto& p : renderPresets()) {
        const RenderVolume& v = p.applies == RenderPreset::Applies::Ct ? *ct : *mr;
        const ResolvedPreset r = resolvePreset(p, v);
        REQUIRE(r.tf.points.size() >= 2);
        REQUIRE(r.windowWidth > 0.0);
        REQUIRE(std::is_sorted(r.tf.points.begin(), r.tf.points.end(),
                               [](const TfPoint& a, const TfPoint& b) { return a.value < b.value; }));
        REQUIRE(findRenderPreset(p.key) == &p);
    }
    REQUIRE(findRenderPreset("não existe") == nullptr);
}

TEST_CASE("Câmera 3D: vistas padrão e projeção", "[render3d]") {
    Camera3D cam;
    for (ViewDirection d : {ViewDirection::Anterior, ViewDirection::Posterior, ViewDirection::Left,
                            ViewDirection::Right, ViewDirection::Superior, ViewDirection::Inferior}) {
        cam.setView(d);
        REQUIRE(cam.right.cross(cam.down).dot(cam.forward) == Approx(1.0));  // right-handed
    }
    cam.setView(ViewDirection::Anterior);
    REQUIRE(cam.forward.y == Approx(1.0));  // looking from the front, towards the back
    REQUIRE(cam.right.x == Approx(1.0));    // the patient's left on the right of the image
    REQUIRE(cam.down.z == Approx(-1.0));    // head at the top
    cam.setView(ViewDirection::Inferior);   // as the axial images
    REQUIRE(cam.right.x == Approx(1.0));
    REQUIRE(cam.down.y == Approx(1.0));

    cam.setView(ViewDirection::Anterior);
    cam.target = {10, 20, 30};
    cam.mmPerPixel = 0.5;
    cam.rotate(0.3, -0.2, 0.1);
    for (bool persp : {false, true}) {
        cam.perspective = persp;
        const Ray r = cam.ray(37.5, 12.5, 100, 80);
        const Vec3 p = r.origin + r.dir * (persp ? 140.0 : 1e5 + 7.0);
        double px = 0;
        double py = 0;
        REQUIRE(cam.project(p, 100, 80, px, py));
        REQUIRE(px == Approx(37.5).margin(1e-6));
        REQUIRE(py == Approx(12.5).margin(1e-6));
    }
    // Zoom keeps the point under the cursor.
    cam.perspective = false;
    const Ray before = cam.ray(80, 10, 100, 80);
    cam.zoom(2.0, 80, 10, 100, 80);
    const Ray after = cam.ray(80, 10, 100, 80);
    REQUIRE((after.origin - before.origin).dot(cam.right) == Approx(0.0).margin(1e-9));
    REQUIRE((after.origin - before.origin).dot(cam.down) == Approx(0.0).margin(1e-9));
}

TEST_CASE("Ray casting: MIP é o máximo ao longo do raio", "[render3d]") {
    // A bright line along y (the viewing direction of the anterior view).
    const int n = 24;
    std::vector<float> values(static_cast<std::size_t>(n) * n * n, -1000.0F);
    for (int y = 0; y < n; ++y) {
        values[static_cast<std::size_t>((12 * n + y) * n + 6)] = 800.0F;  // x = 6, z = 12
    }
    const auto v = RenderVolume::fromValues(n, n, n, 1, 1, 1, values, true);
    Camera3D cam;
    cam.setView(ViewDirection::Anterior);
    cam.target = v->center();
    cam.mmPerPixel = 1.0;
    RenderSettings s;
    s.mode = RenderMode::Mip;
    s.windowCenter = -100.0;
    s.windowWidth = 1800.0;  // -1000 -> 0, 800 -> 255
    s.jitter = false;
    const auto img = render(*v, TransferFunction{}, cam, s, n);
    // Pixel (x, row): image x follows patient x, image rows go down from the head.
    const int row = n - 1 - 12;
    REQUIRE(gray(img[static_cast<std::size_t>(row * n + 6)]) >= 250);
    REQUIRE(gray(img[static_cast<std::size_t>(row * n + 15)]) <= 5);
    REQUIRE(gray(img[static_cast<std::size_t>(3 * n + 6)]) <= 5);
    // Skipping blocks below the running maximum changes nothing.
    s.emptySpaceSkipping = false;
    REQUIRE(render(*v, TransferFunction{}, cam, s, n) == img);
}

TEST_CASE("Ray casting: volume e superfície de uma esfera", "[render3d]") {
    const auto v = ball(40, 12.0, 1000.0F, -1000.0F);
    Camera3D cam;
    cam.setView(ViewDirection::Anterior);
    cam.fit(*v, 64, 64);
    RenderSettings s;
    s.shading = true;
    const auto vr = render(*v, boneLike(), cam, s, 64);
    // Silhouette: the centre is lit, the corners show the background.
    REQUIRE(gray(vr[32 * 64 + 32]) > 100);
    REQUIRE(vr[0] == 0xFF000000U);
    // Empty-space skipping gives the same picture.
    RenderSettings noSkip = s;
    noSkip.emptySpaceSkipping = false;
    const auto vr2 = render(*v, boneLike(), cam, noSkip, 64);
    int worst = 0;
    for (std::size_t i = 0; i < vr.size(); ++i) {
        worst = std::max(worst, std::abs(gray(vr[i]) - gray(vr2[i])));
    }
    REQUIRE(worst <= 1);

    // Surface mode: the hit lies on the sphere.
    s.mode = RenderMode::Surface;
    s.surfaceValue = 0.0;
    const TransferLut lut = buildTransferLut(boneLike(), *v, s.stepMm(v->minSpacing()));
    const auto hit = CpuRayCaster::pick(*v, lut, cam, s, 64, 64, 32.0, 32.0);
    REQUIRE(hit.has_value());
    REQUIRE(distance(*hit, v->center()) == Approx(12.4).margin(1.0));
    REQUIRE((*hit - v->center()).dot(cam.forward) < 0.0);  // the near side
    REQUIRE_FALSE(CpuRayCaster::pick(*v, lut, cam, s, 64, 64, 1.0, 1.0).has_value());
    // Volume mode picks where the ray becomes opaque: also near the surface.
    s.mode = RenderMode::Volume;
    const auto hitVr = CpuRayCaster::pick(*v, lut, cam, s, 64, 64, 32.0, 32.0);
    REQUIRE(hitVr.has_value());
    REQUIRE(distance(*hitVr, v->center()) == Approx(12.4).margin(1.5));
}

TEST_CASE("Ray casting: caixa de recorte", "[render3d]") {
    const auto v = ball(40, 15.0, 1000.0F, -1000.0F);
    Camera3D cam;
    cam.setView(ViewDirection::Anterior);
    cam.fit(*v, 64, 64);
    RenderSettings s;
    s.shading = false;
    s.crop.hi[0] = 0.5;  // keeps the right half of the patient (x < centre)
    const auto img = render(*v, boneLike(), cam, s, 64);
    REQUIRE(gray(img[32 * 64 + 22]) > 100);  // image left = patient right: kept
    REQUIRE(gray(img[32 * 64 + 42]) == 0);   // removed
}

TEST_CASE("Tesoura 3D remove o que foi contornado, só no volume copiado", "[render3d]") {
    const auto mask = rasterizePolygon({{10, 10}, {20, 10}, {20, 30}, {10, 30}}, 40, 40);
    int count = 0;
    for (auto m : mask) {
        count += m;
    }
    REQUIRE(count == 10 * 20);

    const auto v = ball(32, 14.0, 1000.0F, -1000.0F);
    Camera3D cam;
    cam.setView(ViewDirection::Anterior);
    cam.fit(*v, 64, 64);
    SculptCut cut;
    cut.camera = cam;
    cut.width = 64;
    cut.height = 64;
    cut.polygon = {{0, 0}, {32, 0}, {32, 64}, {0, 64}};  // left half of the image
    const auto inside = applyCuts(*v, {cut});
    const int c = 16;
    REQUIRE(inside->at(8, c, c) == 0);  // patient right (image left): removed
    REQUIRE(inside->at(24, c, c) == v->at(24, c, c));
    REQUIRE(inside->at(24, 2, c) == v->at(24, 2, c));  // whole depth kept on the other side
    REQUIRE(v->at(8, c, c) != 0);                      // the original is untouched
    cut.removeInside = false;
    const auto outside = applyCuts(*v, {cut});
    REQUIRE(outside->at(8, c, c) == v->at(8, c, c));
    REQUIRE(outside->at(24, c, c) == 0);
    // Blocks follow the new voxels (skipping stays correct).
    int emptyBlocks = 0;
    for (int b = 0; b < inside->blocksX() * inside->blocksY() * inside->blocksZ(); ++b) {
        emptyBlocks += inside->blockMax(b) == 0 ? 1 : 0;
    }
    REQUIRE(emptyBlocks > 0);
}
