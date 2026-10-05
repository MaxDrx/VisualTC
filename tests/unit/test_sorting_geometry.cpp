#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>

#include "dicom/DicomGeometry.h"
#include "dicom/DicomSorter.h"
#include "dicom/DicomStudy.h"

using namespace vtc;

namespace {

struct SliceSpec {
    Vec3 position;
    Vec3 row{1, 0, 0};
    Vec3 col{0, 1, 0};
    std::optional<int> instance;
    std::optional<int> echo;
    int rows = 64;
    int cols = 64;
    bool spatial = true;
    int frames = 1;
    std::string series = "1.2.3.4";
    double spacing = 0.7;
};

std::shared_ptr<InstanceInfo> makeInstance(const SliceSpec& s, int uniq) {
    auto inst = std::make_shared<InstanceInfo>();
    inst->filePath = "/virtual/" + std::to_string(uniq);
    inst->sopInstanceUid = "1.2.3.4.5." + std::to_string(uniq);
    inst->studyInstanceUid = "1.2.3";
    inst->seriesInstanceUid = s.series;
    inst->frameOfReferenceUid = "1.2.3.9";
    inst->modality = "CT";
    inst->patientName = "TESTE";
    inst->rows = s.rows;
    inst->columns = s.cols;
    inst->bitsAllocated = 16;
    inst->bitsStored = 12;
    inst->samplesPerPixel = 1;
    inst->photometricInterpretation = "MONOCHROME2";
    inst->hasPixelData = true;
    inst->instanceNumber = s.instance;
    inst->numberOfFrames = s.frames;
    for (int f = 0; f < s.frames; ++f) {
        FrameInfo fi;
        fi.frameIndex = f;
        if (s.spatial) {
            fi.geometry.hasPosition = true;
            fi.geometry.hasOrientation = true;
            fi.geometry.position = s.position;
            fi.geometry.rowDir = s.row;
            fi.geometry.colDir = s.col;
            fi.geometry.spacingSource = SpacingSource::PixelSpacing;
            fi.geometry.spacingX = s.spacing;
            fi.geometry.spacingY = s.spacing;
        }
        fi.keys.echoNumber = s.echo;
        inst->frames.push_back(fi);
    }
    return inst;
}

std::vector<FrameRef> refs(const std::vector<std::shared_ptr<InstanceInfo>>& insts) {
    std::vector<FrameRef> out;
    for (const auto& i : insts) {
        for (int f = 0; f < i->numberOfFrames; ++f) {
            out.push_back({i, f});
        }
    }
    return out;
}

std::vector<double> zOf(const std::vector<FrameRef>& frames) {
    std::vector<double> z;
    for (const auto& f : frames) {
        z.push_back(f.geometry().position.z);
    }
    return z;
}

}  // namespace

TEST_CASE("Spatial sort ignores misleading InstanceNumbers", "[sort]") {
    // Instance numbers are random and unrelated to position.
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    std::vector<int> numbers{7, 2, 9, 1, 5, 3, 8, 4, 6, 10};
    for (int i = 0; i < 10; ++i) {
        insts.push_back(makeInstance({{0, 0, i * 2.5}, {1, 0, 0}, {0, 1, 0}, numbers[static_cast<size_t>(i)]}, i));
    }
    std::shuffle(insts.begin(), insts.end(), std::mt19937(42));
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 1);
    const auto z = zOf(stacks[0].frames);
    // Must be monotonic in space (either direction).
    const bool asc = std::is_sorted(z.begin(), z.end());
    const bool desc = std::is_sorted(z.rbegin(), z.rend());
    REQUIRE((asc || desc));
    const auto g = analyzeStack(stacks[0].frames);
    REQUIRE(g.volumetric);
    REQUIRE(g.sliceSpacing == Catch::Approx(2.5));
    REQUIRE(g.uniformSpacing);
}

TEST_CASE("Scroll direction follows the scanner numbering (head-first and feet-first)", "[sort]") {
    for (bool feetFirst : {false, true}) {
        std::vector<std::shared_ptr<InstanceInfo>> insts;
        for (int i = 0; i < 20; ++i) {
            // head-first: image 1 is the most superior slice (z descending)
            const double z = feetFirst ? -100.0 + i * 5.0 : 100.0 - i * 5.0;
            insts.push_back(makeInstance({{0, 0, z}, {1, 0, 0}, {0, 1, 0}, i + 1}, i));
        }
        std::shuffle(insts.begin(), insts.end(), std::mt19937(7));
        const auto stacks = buildStacks(refs(insts));
        REQUIRE(stacks.size() == 1);
        REQUIRE(stacks[0].frames.front().instance->instanceNumber == 1);
        REQUIRE(stacks[0].frames.back().instance->instanceNumber == 20);
        const auto z = zOf(stacks[0].frames);
        if (feetFirst) {
            REQUIRE(std::is_sorted(z.begin(), z.end()));
        } else {
            REQUIRE(std::is_sorted(z.rbegin(), z.rend()));
        }
    }
}

TEST_CASE("Oblique stacks sort along their own normal", "[sort]") {
    const double a = 0.3;
    const Vec3 row{std::cos(a), std::sin(a), 0};
    const Vec3 col{0, 0, -1};
    const Vec3 n = row.cross(col).normalized();
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    for (int i = 0; i < 8; ++i) {
        insts.push_back(makeInstance({n * (i * 3.0) + Vec3{5, 7, 9}, row, col, 8 - i}, i));
    }
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 1);
    const auto g = analyzeStack(stacks[0].frames);
    REQUIRE(g.volumetric);
    REQUIRE(g.sliceSpacing == Catch::Approx(3.0));
    REQUIRE(g.maxInPlaneShift < 1e-6);
}

TEST_CASE("Gantry tilt is detected and kept volumetric", "[geometry]") {
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    const double tilt = 15.0 * 3.14159265358979 / 180.0;
    for (int i = 0; i < 10; ++i) {
        // origins shift in y while slices stay axial (classic tilted CT)
        insts.push_back(makeInstance({{0, i * 5.0 * std::tan(tilt), i * 5.0}, {1, 0, 0}, {0, 1, 0}, i + 1}, i));
    }
    const auto stacks = buildStacks(refs(insts));
    const auto g = analyzeStack(stacks[0].frames);
    REQUIRE(g.has(GeometryIssue::GantryTilt));
    REQUIRE(g.tiltDegrees == Catch::Approx(15.0).margin(0.01));
    REQUIRE(g.volumetric);
}

TEST_CASE("Missing slices and irregular spacing are reported", "[geometry]") {
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    const std::vector<double> zs{0, 1, 2, 3, 6, 7, 8};  // gap between 3 and 6
    for (size_t i = 0; i < zs.size(); ++i) {
        insts.push_back(makeInstance({{0, 0, zs[i]}, {1, 0, 0}, {0, 1, 0}, int(i) + 1}, int(i)));
    }
    auto g = analyzeStack(buildStacks(refs(insts))[0].frames);
    REQUIRE(g.has(GeometryIssue::MissingSlices));
    REQUIRE(g.gapCount == 1);
    REQUIRE(g.volumetric);  // allowed, gaps are rendered as empty in MPR

    insts.clear();
    const std::vector<double> irregular{0, 1.0, 2.2, 3.1, 4.3, 5.2};
    for (size_t i = 0; i < irregular.size(); ++i) {
        insts.push_back(makeInstance({{0, 0, irregular[i]}, {1, 0, 0}, {0, 1, 0}, int(i) + 1}, int(i)));
    }
    g = analyzeStack(buildStacks(refs(insts))[0].frames);
    REQUIRE(g.has(GeometryIssue::IrregularSpacing));
    REQUIRE_FALSE(g.uniformSpacing);
}

TEST_CASE("Duplicate positions explained by echo number split the series", "[sort]") {
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    int u = 0;
    for (int echo = 1; echo <= 2; ++echo) {
        for (int i = 0; i < 6; ++i) {
            insts.push_back(makeInstance({{0, 0, i * 4.0}, {1, 0, 0}, {0, 1, 0}, u + 1, echo}, u));
            ++u;
        }
    }
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 2);
    REQUIRE(stacks[0].label == "Eco 1");
    REQUIRE(stacks[1].label == "Eco 2");
    for (const auto& s : stacks) {
        REQUIRE(s.frames.size() == 6);
        REQUIRE(analyzeStack(s.frames).volumetric);
    }
}

TEST_CASE("Unexplained duplicate slices block volumetric use", "[geometry]") {
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    for (int i = 0; i < 6; ++i) {
        insts.push_back(makeInstance({{0, 0, (i % 3) * 4.0}, {1, 0, 0}, {0, 1, 0}, i + 1}, i));
    }
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 1);
    const auto g = analyzeStack(stacks[0].frames);
    REQUIRE(g.has(GeometryIssue::DuplicatePositions));
    REQUIRE_FALSE(g.volumetric);
}

TEST_CASE("Three-plane localizer stays one non-volumetric stack", "[sort]") {
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    insts.push_back(makeInstance({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 1}, 1));    // axial
    insts.push_back(makeInstance({{0, 0, 0}, {1, 0, 0}, {0, 0, -1}, 2}, 2));   // coronal
    insts.push_back(makeInstance({{0, 0, 0}, {0, 1, 0}, {0, 0, -1}, 3}, 3));   // sagittal
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 1);
    REQUIRE(stacks[0].frames.size() == 3);
    REQUIRE(stacks[0].frames[0].instance->instanceNumber == 1);
    const auto g = analyzeStack(stacks[0].frames);
    REQUIRE_FALSE(g.volumetric);
    REQUIRE(g.has(GeometryIssue::MixedOrientation));
}

TEST_CASE("Series mixing two real stacks is split by orientation", "[sort]") {
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    int u = 0;
    for (int i = 0; i < 5; ++i, ++u) {
        insts.push_back(makeInstance({{0, 0, i * 3.0}, {1, 0, 0}, {0, 1, 0}, u + 1}, u));
    }
    for (int i = 0; i < 4; ++i, ++u) {
        insts.push_back(makeInstance({{0, i * 3.0, 0}, {1, 0, 0}, {0, 0, -1}, u + 1}, u));
    }
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 2);
    REQUIRE(stacks[0].label == "Axial");
    REQUIRE(stacks[1].label == "Coronal");
}

TEST_CASE("Stacks with different pixel spacing are never merged", "[sort]") {
    // Same series, orientation and matrix, but two reconstructions with
    // different fields of view (0.7 mm and 0.5 mm pixels) interleaved in z.
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    for (int i = 0; i < 10; ++i) {
        SliceSpec a{{0, 0, i * 5.0}, {1, 0, 0}, {0, 1, 0}, i + 1};
        a.spacing = 0.7;
        SliceSpec b{{0, 0, i * 5.0 + 2.5}, {1, 0, 0}, {0, 1, 0}, 100 + i};
        b.spacing = 0.5;
        insts.push_back(makeInstance(a, i));
        insts.push_back(makeInstance(b, 100 + i));
    }
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 2);
    for (const auto& st : stacks) {
        REQUIRE(st.frames.size() == 10);
        const double sx = st.frames.front().geometry().spacingX;
        for (const auto& f : st.frames) {
            REQUIRE(f.geometry().spacingX == sx);
        }
        const auto g = analyzeStack(st.frames);
        REQUIRE(g.volumetric);
        REQUIRE(g.sliceSpacing == Catch::Approx(5.0));
    }
    // analyzeStack itself refuses a stack whose pixel spacing varies.
    const auto mixed = analyzeStack(refs(insts));
    REQUIRE_FALSE(mixed.volumetric);
}

TEST_CASE("Ultrasound cine clips become separate stacks", "[sort]") {
    std::vector<std::shared_ptr<InstanceInfo>> insts;
    for (int i = 0; i < 3; ++i) {
        SliceSpec s;
        s.spatial = false;
        s.frames = 12;
        s.instance = i + 1;
        insts.push_back(makeInstance(s, i));
    }
    const auto stacks = buildStacks(refs(insts));
    REQUIRE(stacks.size() == 3);
    REQUIRE(stacks[0].label == "Clipe 1");
    REQUIRE(stacks[0].frames.size() == 12);
    REQUIRE(stacks[0].frames[3].frame == 3);
}

TEST_CASE("Database groups patients, studies and series and drops duplicates", "[db]") {
    StudyDatabase db;
    std::vector<InstancePtr> insts;
    for (int i = 0; i < 5; ++i) {
        insts.push_back(makeInstance({{0, 0, i * 1.0}, {1, 0, 0}, {0, 1, 0}, i + 1, std::nullopt, 64, 64, true, 1,
                                      "1.2.3.4"},
                                     i));
    }
    for (int i = 0; i < 3; ++i) {
        insts.push_back(makeInstance({{0, 0, i * 1.0}, {1, 0, 0}, {0, 1, 0}, i + 1, std::nullopt, 64, 64, true, 1,
                                      "1.2.3.5"},
                                     100 + i));
    }
    REQUIRE(db.addInstances(insts) == 8);
    REQUIRE(db.addInstances(insts) == 0);  // same SOP Instance UIDs again
    REQUIRE(db.duplicateCount() == 8);
    REQUIRE(db.patients().size() == 1);
    REQUIRE(db.patients()[0]->studies.size() == 1);
    REQUIRE(db.patients()[0]->studies[0]->series.size() == 2);
    const auto s = db.findSeries("1.2.3.4");
    REQUIRE(s != nullptr);
    REQUIRE(s->frameCount() == 5);
    REQUIRE(db.studyOf(s->id) != nullptr);
}

TEST_CASE("Segment-plane intersection", "[geometry]") {
    Vec3 out;
    REQUIRE(intersectSegmentWithPlane({0, 0, -1}, {0, 0, 1}, {0, 0, 0.5}, {0, 0, 1}, out));
    REQUIRE(out.z == Catch::Approx(0.5));
    REQUIRE_FALSE(intersectSegmentWithPlane({0, 0, 1}, {0, 0, 2}, {0, 0, 0}, {0, 0, 1}, out));
}
