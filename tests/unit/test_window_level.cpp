#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <set>

#include "imaging/ColorMap.h"
#include "imaging/PixelData.h"
#include "imaging/WindowLevel.h"

using namespace vtc;

namespace {
DecodedFrame makeFrame16(std::vector<std::int16_t> values, int w, int h, double slope = 1.0, double intercept = 0.0) {
    DecodedFrame f;
    f.width = w;
    f.height = h;
    f.format = PixelFormat::I16;
    f.data.resize(values.size() * 2);
    std::memcpy(f.data.data(), values.data(), f.data.size());
    f.slope = slope;
    f.intercept = intercept;
    f.computeRange();
    return f;
}
}  // namespace

TEST_CASE("QA de HU: stored 1000, slope 1, intercept -1024 = -24 HU", "[hu]") {
    DecodedFrame f;
    f.width = 1;
    f.height = 1;
    f.format = PixelFormat::U16;
    f.data.resize(2);
    const std::uint16_t stored = 1000;
    std::memcpy(f.data.data(), &stored, 2);
    f.slope = 1.0;
    f.intercept = -1024.0;
    REQUIRE(f.valueAt(0, 0) == -24.0);
    REQUIRE(std::isnan(f.valueAt(1, 0)));  // outside
}

TEST_CASE("Rescale with non-unit slope", "[hu]") {
    auto f = makeFrame16({100}, 1, 1, 2.5, -10.0);
    REQUIRE(f.valueAt(0, 0) == Catch::Approx(240.0));
}

TEST_CASE("VOI LINEAR follows PS3.3 C.11.2.1.2.1 exactly", "[voi]") {
    // c = 40, w = 400: lower = 40 - 0.5 - 199.5 = -160, upper = 239
    REQUIRE(applyVoi(-160.0, 40, 400, VoiFunction::Linear) == 0.0);
    REQUIRE(applyVoi(-1000.0, 40, 400, VoiFunction::Linear) == 0.0);
    REQUIRE(applyVoi(239.5, 40, 400, VoiFunction::Linear) == 1.0);
    REQUIRE(applyVoi(39.5, 40, 400, VoiFunction::Linear) == Catch::Approx(0.5));
    REQUIRE(applyVoi(-159.0, 40, 400, VoiFunction::Linear) == Catch::Approx((-159.0 - 39.5) / 399.0 + 0.5));
    // width 1 is a hard threshold and never divides by zero
    REQUIRE(applyVoi(10.0, 10.0, 1.0, VoiFunction::Linear) == 1.0);
    REQUIRE(applyVoi(9.0, 10.0, 1.0, VoiFunction::Linear) == 0.0);
}

TEST_CASE("VOI LINEAR_EXACT and SIGMOID", "[voi]") {
    REQUIRE(applyVoi(40.0, 40, 400, VoiFunction::LinearExact) == Catch::Approx(0.5));
    REQUIRE(applyVoi(-160.0, 40, 400, VoiFunction::LinearExact) == 0.0);
    REQUIRE(applyVoi(240.1, 40, 400, VoiFunction::LinearExact) == 1.0);
    REQUIRE(applyVoi(40.0, 40, 400, VoiFunction::Sigmoid) == Catch::Approx(0.5));
    REQUIRE(applyVoi(1e6, 40, 400, VoiFunction::Sigmoid) == Catch::Approx(1.0));
    REQUIRE(parseVoiFunction("SIGMOID") == VoiFunction::Sigmoid);
    REQUIRE(parseVoiFunction("") == VoiFunction::Linear);
}

TEST_CASE("Tabulated VOI LUT", "[voi]") {
    VoiLutTable lut;
    lut.firstMapped = -10;
    lut.bitsPerEntry = 8;
    lut.data = {0, 51, 102, 153, 204, 255};
    REQUIRE(applyVoiLut(-100.0, lut) == 0.0);    // clamps below
    REQUIRE(applyVoiLut(-8.0, lut) == Catch::Approx(102.0 / 255.0));
    REQUIRE(applyVoiLut(1000.0, lut) == 1.0);    // clamps above
}

TEST_CASE("Display renderer applies rescale, window and inversion", "[render]") {
    // HU = raw - 1024; window 0/100 -> -50..50 HU
    auto f = makeFrame16({974, 1024, 1074, 0}, 4, 1, 1.0, -1024.0);
    DisplayRenderer r;
    std::uint8_t out[4];
    DisplayParams p;
    p.center = 0.0;
    p.width = 100.0;
    r.renderGray(f, p, out, 4);
    REQUIRE(out[0] == 0);
    // HU 0 with c=0, w=100: ((0 - (-0.5)) / 99 + 0.5) * 255 = 128.79 -> 129
    REQUIRE(out[1] == 129);
    REQUIRE(out[2] == 255);
    REQUIRE(out[3] == 0);

    p.invert = true;
    r.renderGray(f, p, out, 4);
    REQUIRE(out[0] == 255);
    REQUIRE(out[2] == 0);

    // MONOCHROME1 inverts by itself, and user inversion toggles it back.
    f.monochrome1 = true;
    r.renderGray(f, p, out, 4);
    REQUIRE(out[0] == 0);
    p.invert = false;
    r.renderGray(f, p, out, 4);
    REQUIRE(out[0] == 255);
}

TEST_CASE("Float frames render NaN as black", "[render]") {
    DecodedFrame f;
    f.width = 2;
    f.height = 1;
    f.format = PixelFormat::F32;
    f.data.resize(8);
    const float vals[2] = {std::numeric_limits<float>::quiet_NaN(), 1000.0F};
    std::memcpy(f.data.data(), vals, 8);
    DisplayRenderer r;
    std::uint8_t out[2];
    DisplayParams p;
    p.center = 0;
    p.width = 100;
    r.renderGray(f, p, out, 2);
    REQUIRE(out[0] == 0);
    REQUIRE(out[1] == 255);
}

TEST_CASE("Auto window uses robust percentiles", "[voi]") {
    std::vector<std::int16_t> v(1000);
    for (int i = 0; i < 1000; ++i) {
        v[static_cast<size_t>(i)] = static_cast<std::int16_t>(i);
    }
    v[0] = -30000;  // outlier
    auto f = makeFrame16(v, 1000, 1);
    const auto w = autoWindow(f);
    REQUIRE(w.center > 400.0);
    REQUIRE(w.center < 600.0);
    REQUIRE(w.width < 1200.0);
}

TEST_CASE("CT presets contain clinically expected windows", "[voi]") {
    bool lung = false;
    for (const auto& p : ctPresets()) {
        if (p.name == "Pulmão") {
            lung = p.center == -600.0 && p.width == 1500.0;
        }
        REQUIRE(p.width >= 1.0);
    }
    REQUIRE(lung);
}

TEST_CASE("Colour tables (LUT) for greyscale images", "[display]") {
    auto rgb = [](std::uint32_t c) { return std::array<int, 3>{int((c >> 16) & 0xFF), int((c >> 8) & 0xFF), int(c & 0xFF)}; };
    // Grey is the identity: the default display is unchanged.
    const auto& gray = colorMapTable(ColorMap::Gray);
    for (int i = 0; i < 256; ++i) {
        const auto c = gray[static_cast<size_t>(i)];
        REQUIRE((c >> 24) == 0xFFu);
        REQUIRE(rgb(c) == std::array<int, 3>{i, i, i});
    }
    std::set<std::string> names;
    for (ColorMap m : kColorMaps) {
        const auto& t = colorMapTable(m);
        REQUIRE(&t == &colorMapTable(m));  // built once, then cached
        names.insert(colorMapName(m));
        for (auto c : t) {
            REQUIRE((c >> 24) == 0xFFu);  // opaque
        }
        if (m != ColorMap::Gray) {
            bool coloured = false;
            for (auto c : t) {
                const auto v = rgb(c);
                coloured = coloured || v[0] != v[1] || v[1] != v[2];
            }
            REQUIRE(coloured);
        }
    }
    REQUIRE(names.size() == kColorMaps.size());
    REQUIRE(names.count("") == 0);
    // Hot iron: black -> red -> orange -> white.
    const auto& hot = colorMapTable(ColorMap::HotIron);
    REQUIRE(rgb(hot[0]) == std::array<int, 3>{0, 0, 0});
    REQUIRE(rgb(hot[255]) == std::array<int, 3>{255, 255, 255});
    const auto mid = rgb(hot[128]);
    REQUIRE(mid[0] == 255);
    REQUIRE(mid[1] < 10);
    REQUIRE(mid[2] == 0);
    // Brightness never decreases along the hot iron and grey scales.
    int last = -1;
    for (auto c : hot) {
        const auto v = rgb(c);
        const int sum = v[0] + v[1] + v[2];
        REQUIRE(sum >= last);
        last = sum;
    }
}
