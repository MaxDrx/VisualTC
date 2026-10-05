#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>

#include "core/PathUtil.h"
#include "dicom/DicomDecoder.h"
#include "dicom/DicomParser.h"
#include "dicom/DicomScanner.h"
#include "dicom/DicomStudy.h"
#include "fixtures/DicomFixtures.h"

using namespace vtc;
using namespace vtc::test;
namespace fs = std::filesystem;

namespace {

SyntheticImage ctSlice(int index, int rows = 32, int cols = 32) {
    SyntheticImage img;
    img.rows = rows;
    img.columns = cols;
    img.bitsStored = 16;
    img.highBit = 15;
    img.pixelRepresentation = 1;
    img.pixels.resize(static_cast<size_t>(rows * cols));
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            // stored = HU + 1024 ; HU = 10*x - 5*y + index
            const int hu = 10 * x - 5 * y + index;
            img.pixels[static_cast<size_t>(y * cols + x)] = static_cast<std::uint16_t>(static_cast<std::int16_t>(hu + 1024));
        }
    }
    img.slope = 1.0;
    img.intercept = -1024.0;
    img.windowCenter = 40.0;
    img.windowWidth = 400.0;
    img.position = {-80.0, -80.0, index * 2.0};
    img.instanceNumber = 100 - index;  // deliberately reversed numbering
    img.sliceThickness = 2.0;
    return img;
}

std::string randomName(std::mt19937& gen) {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string s;
    for (int i = 0; i < 12; ++i) {
        s.push_back(chars[gen() % 36]);
    }
    return s;  // no extension
}

}  // namespace

TEST_CASE("Scanner finds DICOM files without extension in nested folders", "[io]") {
    TempDir dir;
    std::mt19937 gen(1);
    fs::create_directories(dir.path() / "DICOM" / "ST0" / "SE1");
    fs::create_directories(dir.path() / "outro");
    for (int i = 0; i < 12; ++i) {
        const auto sub = i % 2 == 0 ? dir.path() / "DICOM" / "ST0" / "SE1" : dir.path() / "outro";
        REQUIRE(writeDicom(sub / randomName(gen), ctSlice(i)));
    }
    // Non-DICOM noise
    std::ofstream(dir.path() / "LEIAME.txt") << "nao e dicom";
    std::ofstream(dir.path() / ".DS_Store") << "x";
    {
        std::ofstream junk(dir.path() / "junk.bin", std::ios::binary);
        std::mt19937 g2(3);
        for (int i = 0; i < 5000; ++i) {
            junk.put(static_cast<char>(g2()));
        }
    }
    // Truncated DICOM: valid header, cut in the middle of pixel data
    {
        const auto full = dir.path() / "full.tmp";
        REQUIRE(writeDicom(full, ctSlice(50, 64, 64)));
        std::ifstream in(full, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        fs::remove(full);
        std::ofstream out(dir.path() / "truncado", std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
    }

    DicomScanner scanner;
    const auto result = scanner.scan({dir.path()});
    // The truncated file is detected structurally (its Pixel Data length
    // exceeds the file) and reported as an issue instead of being shown with
    // silently zero-filled pixels.
    REQUIRE(result.instances.size() == 12);
    REQUIRE(result.nonDicomFiles >= 2);

    StudyDatabase db;
    db.addInstances(result.instances);
    REQUIRE(db.patients().size() == 1);
    const auto series = db.allSeries();
    REQUIRE(series.size() == 1);
    const auto s = series.front();
    REQUIRE(s->frameCount() == 12);
    REQUIRE(s->geometry.volumetric);
    REQUIRE(s->geometry.sliceSpacing == Catch::Approx(2.0));
    // reversed InstanceNumber -> display starts at the highest z
    REQUIRE(s->frames.front().instance->instanceNumber == 89);

    bool sawTruncated = false;
    for (const auto& issue : result.issues) {
        if (issue.filePath.find("truncado") != std::string::npos) {
            sawTruncated = true;
            REQUIRE(issue.status == ParseStatus::Malformed);
        }
    }
    REQUIRE(sawTruncated);
}

TEST_CASE("Header parsing extracts geometry, rescale and windows", "[io]") {
    TempDir dir;
    auto img = ctSlice(3);
    img.rowDir = {0, 1, 0};
    img.colDir = {0, 0, -1};
    img.spacingRow = 0.8;     // between rows
    img.spacingColumn = 0.6;  // between columns
    REQUIRE(writeDicom(dir.path() / "a", img));
    const auto r = parseDicomHeader(dir.path() / "a");
    REQUIRE(r.status == ParseStatus::Ok);
    const auto& inst = *r.instance;
    REQUIRE(inst.rows == 32);
    REQUIRE(inst.modality == "CT");
    REQUIRE(inst.instanceNumber == 97);
    const auto& g = inst.frames[0].geometry;
    REQUIRE(g.hasPosition);
    REQUIRE(g.hasOrientation);
    REQUIRE(g.spacingY == Catch::Approx(0.8));
    REQUIRE(g.spacingX == Catch::Approx(0.6));
    REQUIRE(g.rowDir.y == Catch::Approx(1.0));
    REQUIRE(g.normal().x == Catch::Approx(-1.0));
    REQUIRE(inst.frames[0].rescaleIntercept == -1024.0);
    REQUIRE(inst.frames[0].windows.size() == 1);
    REQUIRE(inst.frames[0].windows[0].width == 400.0);
    REQUIRE(g.sliceThickness.value() == 2.0);
}

TEST_CASE("Decoded CT values give exact Hounsfield Units", "[io][hu]") {
    TempDir dir;
    REQUIRE(writeDicom(dir.path() / "ct", ctSlice(7)));
    const auto r = parseDicomHeader(dir.path() / "ct");
    REQUIRE(r.status == ParseStatus::Ok);
    const auto dec = decodeInstance(*r.instance);
    REQUIRE(dec.ok());
    const auto& f = *dec.frames[0];
    REQUIRE(f.format == PixelFormat::I16);
    for (int y = 0; y < 32; y += 7) {
        for (int x = 0; x < 32; x += 5) {
            REQUIRE(f.valueAt(x, y) == Catch::Approx(10 * x - 5 * y + 7));
        }
    }
}

TEST_CASE("Compressed transfer syntaxes decode identically to the original", "[io][codec]") {
    TempDir dir;
    auto img = ctSlice(4, 48, 40);
    // add signed values well below zero (air) to exercise sign handling
    img.pixels[0] = static_cast<std::uint16_t>(static_cast<std::int16_t>(-1000 + 1024 - 2000));
    REQUIRE(writeDicom(dir.path() / "raw", img));
    const auto rawInst = parseDicomHeader(dir.path() / "raw");
    REQUIRE(rawInst.status == ParseStatus::Ok);
    const auto reference = decodeInstance(*rawInst.instance);
    REQUIRE(reference.ok());

    const std::vector<std::pair<Encoding, std::string>> encodings{
        {Encoding::ImplicitLittle, "1.2.840.10008.1.2"},
        {Encoding::JpegLossless, "1.2.840.10008.1.2.4.70"},
        {Encoding::JpegLsLossless, "1.2.840.10008.1.2.4.80"},
        {Encoding::Jpeg2000Lossless, "1.2.840.10008.1.2.4.90"},
        {Encoding::RleLossless, "1.2.840.10008.1.2.5"},
    };
    for (const auto& [enc, uid] : encodings) {
        INFO("transfer syntax " << uid);
        const auto file = dir.path() / ("enc_" + uid);
        REQUIRE(writeDicom(file, img, enc));
        const auto parsed = parseDicomHeader(file);
        REQUIRE(parsed.status == ParseStatus::Ok);
        REQUIRE(parsed.instance->transferSyntaxUid == uid);
        const auto dec = decodeInstance(*parsed.instance);
        REQUIRE(dec.ok());
        const auto& a = *reference.frames[0];
        const auto& b = *dec.frames[0];
        REQUIRE(a.width == b.width);
        REQUIRE(a.height == b.height);
        for (int y = 0; y < a.height; ++y) {
            for (int x = 0; x < a.width; ++x) {
                REQUIRE(a.valueAt(x, y) == b.valueAt(x, y));
            }
        }
    }
}

TEST_CASE("Raw dataset without preamble or meta header is recognized", "[io]") {
    TempDir dir;
    REQUIRE(writeDicom(dir.path() / "withheader", ctSlice(1), Encoding::ImplicitLittle));
    REQUIRE(stripPart10Header(dir.path() / "withheader", dir.path() / "IM0001"));
    REQUIRE(looksLikeDicom(dir.path() / "IM0001"));
    const auto r = parseDicomHeader(dir.path() / "IM0001");
    REQUIRE(r.status == ParseStatus::Ok);
    const auto dec = decodeInstance(*r.instance);
    REQUIRE(dec.ok());
    REQUIRE(dec.frames[0]->valueAt(3, 2) == Catch::Approx(10 * 3 - 5 * 2 + 1));
}

TEST_CASE("BitsStored 12 with garbage in the high bits is masked and sign-extended", "[io][bits]") {
    TempDir dir;
    SyntheticImage img;
    img.rows = 2;
    img.columns = 2;
    img.bitsStored = 12;
    img.highBit = 11;
    img.pixelRepresentation = 1;
    // -1 in 12 bits = 0xFFF; -2048 = 0x800; 2047 = 0x7FF; 5 = 0x005.
    // Upper nibble filled with garbage (e.g. overlay bits): 0xA000.
    img.pixels = {0xAFFF, 0xA800, 0xA7FF, 0xA005};
    img.slope = 1.0;
    img.intercept = 0.0;
    REQUIRE(writeDicom(dir.path() / "bits", img));
    const auto r = parseDicomHeader(dir.path() / "bits");
    REQUIRE(r.status == ParseStatus::Ok);
    const auto dec = decodeInstance(*r.instance);
    REQUIRE(dec.ok());
    const auto& f = *dec.frames[0];
    REQUIRE(f.valueAt(0, 0) == -1.0);
    REQUIRE(f.valueAt(1, 0) == -2048.0);
    REQUIRE(f.valueAt(0, 1) == 2047.0);
    REQUIRE(f.valueAt(1, 1) == 5.0);

    // Unsigned 12-bit with garbage
    img.pixelRepresentation = 0;
    REQUIRE(writeDicom(dir.path() / "ubits", img));
    const auto ru = parseDicomHeader(dir.path() / "ubits");
    const auto du = decodeInstance(*ru.instance);
    REQUIRE(du.ok());
    REQUIRE(du.frames[0]->valueAt(0, 0) == 4095.0);
    REQUIRE(du.frames[0]->valueAt(1, 0) == 2048.0);
}

TEST_CASE("Latin-1 patient names are converted to UTF-8", "[io][charset]") {
    TempDir dir;
    auto img = ctSlice(0);
    img.specificCharacterSet = "ISO_IR 100";
    img.patientNameRaw = std::string("CONCEI\xC7\xC3O^MARIA");  // CONCEIÇÃO
    REQUIRE(writeDicom(dir.path() / "nome", img));
    const auto r = parseDicomHeader(dir.path() / "nome");
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE(r.instance->patientName == "CONCEI\xC3\x87\xC3\x83O MARIA");
}

TEST_CASE("Missing PixelSpacing is never invented", "[io][reliability]") {
    TempDir dir;
    auto img = ctSlice(0);
    img.writeSpacing = false;
    REQUIRE(writeDicom(dir.path() / "nospacing", img));
    const auto r = parseDicomHeader(dir.path() / "nospacing");
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE_FALSE(r.instance->frames[0].geometry.hasSpacing());
    REQUIRE(r.instance->frames[0].geometry.spacingSource == SpacingSource::None);
}

TEST_CASE("8-bit RGB images decode to interleaved RGB", "[io][color]") {
    TempDir dir;
    SyntheticImage img;
    img.rows = 4;
    img.columns = 4;
    img.samplesPerPixel = 3;
    img.photometric = "RGB";
    img.bitsAllocated = 8;
    img.bitsStored = 8;
    img.highBit = 7;
    img.pixelRepresentation = 0;
    img.modality = "US";
    img.sopClassUid = "1.2.840.10008.5.1.4.1.1.6.1";
    img.writePosition = false;
    img.writeSpacing = false;
    for (int i = 0; i < 16; ++i) {
        img.pixels8.push_back(static_cast<std::uint8_t>(i * 10));
        img.pixels8.push_back(200);
        img.pixels8.push_back(7);
    }
    REQUIRE(writeDicom(dir.path() / "rgb", img));
    const auto r = parseDicomHeader(dir.path() / "rgb");
    REQUIRE(r.status == ParseStatus::Ok);
    const auto dec = decodeInstance(*r.instance);
    REQUIRE(dec.ok());
    const auto& f = *dec.frames[0];
    REQUIRE(f.isColor());
    REQUIRE(f.data[3 * 5] == 50);
    REQUIRE(f.data[3 * 5 + 1] == 200);
    REQUIRE(f.data[3 * 5 + 2] == 7);

    // RLE-compressed RGB round trip
    REQUIRE(writeDicom(dir.path() / "rgbrle", img, Encoding::RleLossless));
    const auto rr = parseDicomHeader(dir.path() / "rgbrle");
    const auto dr = decodeInstance(*rr.instance);
    REQUIRE(dr.ok());
    REQUIRE(dr.frames[0]->data == f.data);
}

TEST_CASE("Folders and files with accented names are read", "[io][reliability]") {
    TempDir dir;
    // "Exames/João Conceição/Série ação" in UTF-8, converted at the boundary.
    const fs::path folder = dir.path() / utf8ToPath("Exames") / utf8ToPath("Jo\xC3\xA3o Concei\xC3\xA7\xC3\xA3o") /
                            utf8ToPath("S\xC3\xA9rie a\xC3\xA7\xC3\xA3o");
    fs::create_directories(folder);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(writeDicom(folder / utf8ToPath("imagem-\xC3\xA7" + std::to_string(i)), ctSlice(i)));
    }
    DicomScanner scanner;
    const auto result = scanner.scan({dir.path()});
    REQUIRE(result.instances.size() == 3);
    for (const auto& inst : result.instances) {
        REQUIRE(inst->filePath.find("Jo\xC3\xA3o") != std::string::npos);
        const auto dec = decodeInstance(*inst);
        REQUIRE(dec.ok());
    }
}

TEST_CASE("Hostile headers are rejected before allocation", "[io][security]") {
    TempDir dir;
    auto img = ctSlice(0, 8, 8);
    REQUIRE(writeDicom(dir.path() / "small", img));
    ParseLimits tight;
    tight.maxRows = 4;
    const auto r = parseDicomHeader(dir.path() / "small", tight);
    REQUIRE(r.status == ParseStatus::Unsupported);
    ParseLimits lowMem;
    lowMem.maxPixelBytes = 16;
    REQUIRE(parseDicomHeader(dir.path() / "small", lowMem).status == ParseStatus::Unsupported);
}

TEST_CASE("Random garbage never crashes the parser", "[io][security]") {
    TempDir dir;
    std::mt19937 gen(99);
    // Valid file with random byte corruption at many positions.
    REQUIRE(writeDicom(dir.path() / "base", ctSlice(0, 16, 16)));
    std::ifstream in(dir.path() / "base", std::ios::binary);
    const std::vector<char> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (int trial = 0; trial < 150; ++trial) {
        std::vector<char> bytes = original;
        const int flips = 1 + static_cast<int>(gen() % 8);
        for (int k = 0; k < flips; ++k) {
            const std::size_t pos = 132 + gen() % (bytes.size() - 132);
            bytes[pos] = static_cast<char>(gen());
        }
        if (trial % 10 == 0) {
            bytes.resize(132 + gen() % (bytes.size() - 132));
        }
        const auto path = dir.path() / ("fuzz" + std::to_string(trial));
        {
            std::ofstream out(path, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const auto r = parseDicomHeader(path);
        if (r.status == ParseStatus::Ok) {
            (void)decodeInstance(*r.instance);  // must not crash
        }
    }
    SUCCEED("no crash");
}

// Opt-in (hidden) fuzz harness for the encapsulated codecs. Third-party
// decoders are not guaranteed to survive this in-process (that is why the
// application decodes in visualtc-worker); run it under ASan to audit our
// own code paths:  visualtc_tests "[fuzz]"   (VTC_FUZZ_TRIALS=N, VTC_FUZZ_SEED=S)
TEST_CASE("Corrupted compressed streams (in-process audit)", "[.][fuzz]") {
    TempDir dir;
    const char* env = std::getenv("VTC_FUZZ_TRIALS");
    const int trials = env != nullptr ? std::max(1, std::atoi(env)) : 300;
    const auto encoding = GENERATE(Encoding::JpegBaseline, Encoding::JpegLossless, Encoding::RleLossless);
    SyntheticImage img = ctSlice(0, 24, 24);
    if (encoding == Encoding::JpegBaseline) {
        img.samplesPerPixel = 3;
        img.photometric = "RGB";
        img.bitsAllocated = 8;
        img.bitsStored = 8;
        img.highBit = 7;
        img.pixelRepresentation = 0;
        img.slope.reset();
        img.intercept.reset();
        img.pixels.clear();
        for (int i = 0; i < 24 * 24; ++i) {
            for (int c = 0; c < 3; ++c) {
                img.pixels8.push_back(static_cast<std::uint8_t>((i * (c + 3)) & 0xFF));
            }
        }
    }
    REQUIRE(writeDicom(dir.path() / "base", img, encoding));
    const auto base = parseDicomHeader(dir.path() / "base");
    REQUIRE(base.status == ParseStatus::Ok);
    std::ifstream in(dir.path() / "base", std::ios::binary);
    const std::vector<char> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Corrupt only the Pixel Data element (7FE0,0010) and what follows.
    const char tag[4] = {'\xE0', '\x7F', '\x10', '\x00'};
    const auto it = std::find_end(original.begin(), original.end(), std::begin(tag), std::end(tag));
    REQUIRE(it != original.end());
    const auto from = static_cast<std::size_t>(it - original.begin()) + 4;
    const char* seedEnv = std::getenv("VTC_FUZZ_SEED");
    std::mt19937 gen(seedEnv != nullptr ? static_cast<unsigned>(std::strtoul(seedEnv, nullptr, 10)) : 1234u);
    for (int trial = 0; trial < trials; ++trial) {
        std::vector<char> bytes = original;
        const int flips = 1 + static_cast<int>(gen() % 6);
        for (int k = 0; k < flips; ++k) {
            bytes[from + gen() % (bytes.size() - from)] = static_cast<char>(gen());
        }
        const auto path = dir.path() / ("f" + std::to_string(trial));
        {
            std::ofstream out(path, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const auto r = parseDicomHeader(path);
        if (r.status == ParseStatus::Ok) {
            (void)decodeInstance(*r.instance);
        }
    }
    SUCCEED("no crash");
}

TEST_CASE("Parallel decoding from several threads is safe", "[io][threads]") {
    TempDir dir;
    std::vector<InstancePtr> insts;
    for (int i = 0; i < 8; ++i) {
        const auto p = dir.path() / ("s" + std::to_string(i));
        REQUIRE(writeDicom(p, ctSlice(i), i % 2 == 0 ? Encoding::Jpeg2000Lossless : Encoding::JpegLsLossless));
        insts.push_back(parseDicomHeader(p).instance);
    }
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (const auto& inst : insts) {
                if (decodeInstance(*inst).ok()) {
                    ++ok;
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    REQUIRE(ok == 32);
}

TEST_CASE("Enhanced CT multi-frame uses per-frame functional groups", "[io][enhanced]") {
    TempDir dir;
    EnhancedSpec spec;
    // Frames stored out of spatial order on purpose.
    const std::vector<double> zs{6.0, 0.0, 4.0, 2.0, 8.0};
    for (double z : zs) {
        spec.origins.push_back({-5.0, -5.0, z});
        std::vector<std::int16_t> hu(static_cast<size_t>(spec.rows * spec.columns), static_cast<std::int16_t>(z * 10));
        spec.hu.push_back(hu);
    }
    REQUIRE(writeEnhancedCt(dir.path() / "enhanced", spec));
    const auto r = parseDicomHeader(dir.path() / "enhanced");
    REQUIRE(r.status == ParseStatus::Ok);
    const auto& inst = *r.instance;
    REQUIRE(inst.isEnhanced);
    REQUIRE(inst.numberOfFrames == 5);
    REQUIRE(inst.frames[0].geometry.position.z == Catch::Approx(6.0));
    REQUIRE(inst.frames[1].geometry.position.z == Catch::Approx(0.0));
    REQUIRE(inst.frames[0].geometry.spacingX == Catch::Approx(0.75));
    REQUIRE(inst.frames[0].geometry.hasOrientation);
    REQUIRE(inst.frames[0].rescaleIntercept == -1024.0);
    REQUIRE(inst.frames[0].windows.size() == 1);

    StudyDatabase db;
    db.addInstances({r.instance});
    const auto series = db.allSeries();
    REQUIRE(series.size() == 1);
    const auto& s = *series.front();
    REQUIRE(s.frameCount() == 5);
    REQUIRE(s.geometry.volumetric);
    REQUIRE(s.geometry.sliceSpacing == Catch::Approx(2.0));
    const auto dec = decodeInstance(inst);
    REQUIRE(dec.ok());
    REQUIRE(dec.frames.size() == 5);
    // Display order is spatial; the decoded frame matches its own z.
    for (const auto& fr : s.frames) {
        const double z = fr.geometry().position.z;
        REQUIRE(dec.frames[static_cast<size_t>(fr.frame)]->valueAt(3, 3) == Catch::Approx(z * 10));
    }
}

TEST_CASE("Deflated and big endian transfer syntaxes", "[io][codec]") {
    TempDir dir;
    const auto img = ctSlice(5, 24, 20);
    REQUIRE(writeDicom(dir.path() / "ref", img));
    const auto ref = decodeInstance(*parseDicomHeader(dir.path() / "ref").instance);
    REQUIRE(ref.ok());
    for (const auto& [enc, uid] : std::vector<std::pair<Encoding, std::string>>{
             {Encoding::Deflated, "1.2.840.10008.1.2.1.99"}, {Encoding::ExplicitBig, "1.2.840.10008.1.2.2"}}) {
        INFO("transfer syntax " << uid);
        const auto file = dir.path() / ("ts_" + uid);
        REQUIRE(writeDicom(file, img, enc));
        const auto parsed = parseDicomHeader(file);
        REQUIRE(parsed.status == ParseStatus::Ok);
        REQUIRE(parsed.instance->transferSyntaxUid == uid);
        REQUIRE(parsed.instance->rows == 24);
        REQUIRE(parsed.instance->frames[0].geometry.position.z == Catch::Approx(10.0));
        const auto dec = decodeInstance(*parsed.instance);
        REQUIRE(dec.ok());
        for (int y = 0; y < 24; y += 5) {
            for (int x = 0; x < 20; x += 3) {
                REQUIRE(dec.frames[0]->valueAt(x, y) == ref.frames[0]->valueAt(x, y));
            }
        }
    }
}

TEST_CASE("YBR_FULL and PALETTE COLOR are converted to RGB", "[io][color]") {
    TempDir dir;
    SyntheticImage img;
    img.rows = 2;
    img.columns = 2;
    img.samplesPerPixel = 3;
    img.bitsAllocated = 8;
    img.bitsStored = 8;
    img.highBit = 7;
    img.pixelRepresentation = 0;
    img.writePosition = false;
    img.writeSpacing = false;
    img.modality = "OT";
    img.sopClassUid = "1.2.840.10008.5.1.4.1.1.7";
    // Pure red, green, blue and grey expressed in YBR_FULL.
    const int rgb[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {128, 128, 128}};
    for (const auto& c : rgb) {
        const double y = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
        const double cb = 128 - 0.168736 * c[0] - 0.331264 * c[1] + 0.5 * c[2];
        const double cr = 128 + 0.5 * c[0] - 0.418688 * c[1] - 0.081312 * c[2];
        for (double v : {y, cb, cr}) {
            img.pixels8.push_back(static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)));
        }
    }
    img.photometric = "YBR_FULL";
    REQUIRE(writeDicom(dir.path() / "ybr", img));
    const auto dec = decodeInstance(*parseDicomHeader(dir.path() / "ybr").instance);
    REQUIRE(dec.ok());
    for (int i = 0; i < 4; ++i) {
        for (int c = 0; c < 3; ++c) {
            REQUIRE(std::abs(int(dec.frames[0]->data[static_cast<size_t>(i * 3 + c)]) - rgb[i][c]) <= 2);
        }
    }

    // PALETTE COLOR: 8-bit indices through 256-entry, 16-bit LUTs.
    SyntheticImage pal;
    pal.rows = 1;
    pal.columns = 3;
    pal.bitsAllocated = 8;
    pal.bitsStored = 8;
    pal.highBit = 7;
    pal.pixelRepresentation = 0;
    pal.photometric = "PALETTE COLOR";
    pal.writePosition = false;
    pal.writeSpacing = false;
    pal.modality = "OT";
    pal.sopClassUid = "1.2.840.10008.5.1.4.1.1.7";
    pal.pixels8 = {0, 1, 2};
    pal.paletteRed.assign(256, 0);
    pal.paletteGreen.assign(256, 0);
    pal.paletteBlue.assign(256, 0);
    pal.paletteRed[0] = 0xFFFF;
    pal.paletteGreen[1] = 0xFFFF;
    pal.paletteBlue[2] = 0x8000;
    REQUIRE(writeDicom(dir.path() / "palette", pal));
    const auto pr = parseDicomHeader(dir.path() / "palette");
    REQUIRE(pr.status == ParseStatus::Ok);
    const auto pd = decodeInstance(*pr.instance);
    REQUIRE(pd.ok());
    REQUIRE(pd.frames[0]->isColor());
    const auto& d = pd.frames[0]->data;
    REQUIRE(d[0] == 255);
    REQUIRE(d[1] == 0);
    REQUIRE(d[4] == 255);
    REQUIRE(d[8] == 128);
}

TEST_CASE("JPEG Baseline color decodes to RGB whatever the declared PI", "[io][color][codec]") {
    TempDir dir;
    SyntheticImage img;
    img.rows = 32;
    img.columns = 32;
    img.samplesPerPixel = 3;
    img.photometric = "RGB";
    img.bitsAllocated = 8;
    img.bitsStored = 8;
    img.highBit = 7;
    img.pixelRepresentation = 0;
    img.modality = "US";
    img.sopClassUid = "1.2.840.10008.5.1.4.1.1.6.1";
    img.writePosition = false;
    img.writeSpacing = false;
    // Left half red, right half blue: survives lossy compression clearly.
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const bool left = x < 16;
            img.pixels8.push_back(left ? 220 : 20);
            img.pixels8.push_back(30);
            img.pixels8.push_back(left ? 20 : 220);
        }
    }
    // GDCM's encoder writes a JFIF (YCbCr) stream. The standard header says
    // YBR_FULL_422; some devices wrongly declare RGB for the same stream.
    // Both must display the original colours.
    const auto declared = GENERATE(std::string("YBR_FULL_422"), std::string("RGB"));
    CAPTURE(declared);
    const auto file = dir.path() / "jpeg";
    REQUIRE(writeDicom(file, img, Encoding::JpegBaseline));
    REQUIRE(setPhotometric(file, declared));
    const auto parsed = parseDicomHeader(file);
    REQUIRE(parsed.status == ParseStatus::Ok);
    REQUIRE(parsed.instance->transferSyntaxUid == "1.2.840.10008.1.2.4.50");
    REQUIRE(parsed.instance->photometricInterpretation == declared);
    REQUIRE(parsed.instance->lossyCompressed);
    const auto dec = decodeInstance(*parsed.instance);
    REQUIRE(dec.ok());
    const auto& f = *dec.frames[0];
    REQUIRE(f.isColor());
    auto px = [&f](int x, int y, int c) { return int(f.data[static_cast<size_t>((y * 32 + x) * 3 + c)]); };
    REQUIRE(std::abs(px(4, 10, 0) - 220) < 20);
    REQUIRE(std::abs(px(4, 10, 1) - 30) < 20);
    REQUIRE(std::abs(px(4, 10, 2) - 20) < 20);
    REQUIRE(std::abs(px(28, 10, 0) - 20) < 20);
    REQUIRE(std::abs(px(28, 10, 2) - 220) < 20);
}

TEST_CASE("Compressed colour ignores a wrong Planar Configuration", "[io][color][codec]") {
    // JPEG, JPEG-LS and JPEG 2000 decoders always return interleaved samples;
    // some writers nevertheless declare Planar Configuration = 1.
    TempDir dir;
    SyntheticImage img;
    img.rows = 32;
    img.columns = 32;
    img.samplesPerPixel = 3;
    img.photometric = "RGB";
    img.bitsAllocated = 8;
    img.bitsStored = 8;
    img.highBit = 7;
    img.pixelRepresentation = 0;
    img.modality = "OT";
    img.sopClassUid = "1.2.840.10008.5.1.4.1.1.7";
    img.writePosition = false;
    img.writeSpacing = false;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const bool left = x < 16;
            img.pixels8.push_back(left ? 220 : 20);
            img.pixels8.push_back(30);
            img.pixels8.push_back(left ? 20 : 220);
        }
    }
    const auto encoding = GENERATE(Encoding::JpegLsLossless, Encoding::Jpeg2000Lossless, Encoding::JpegLossless,
                                   Encoding::JpegBaseline);
    CAPTURE(static_cast<int>(encoding));
    const auto file = dir.path() / "planar";
    REQUIRE(writeDicom(file, img, encoding));
    REQUIRE(setUS(file, 0x0028, 0x0006, 1));
    const auto parsed = parseDicomHeader(file);
    REQUIRE(parsed.status == ParseStatus::Ok);
    REQUIRE(parsed.instance->planarConfiguration == 1);
    const auto dec = decodeInstance(*parsed.instance);
    REQUIRE(dec.ok());
    const auto& f = *dec.frames[0];
    auto px = [&f](int x, int y, int c) { return int(f.data[static_cast<size_t>((y * 32 + x) * 3 + c)]); };
    REQUIRE(std::abs(px(4, 10, 0) - 220) < 20);
    REQUIRE(std::abs(px(4, 10, 1) - 30) < 20);
    REQUIRE(std::abs(px(4, 10, 2) - 20) < 20);
    REQUIRE(std::abs(px(28, 20, 0) - 20) < 20);
    REQUIRE(std::abs(px(28, 20, 2) - 220) < 20);
}

TEST_CASE("Nearly orthogonal orientation vectors are orthonormalized", "[io][geometry]") {
    TempDir dir;
    auto img = ctSlice(0);
    // Rounded direction cosines (dot product 0.004): accepted and corrected
    // so that every geometric computation uses an orthonormal basis.
    img.rowDir = {1.0, 0.0, 0.0};
    img.colDir = {0.004, 0.999992, 0.0};
    REQUIRE(writeDicom(dir.path() / "rounded", img));
    const auto ok = parseDicomHeader(dir.path() / "rounded");
    REQUIRE(ok.status == ParseStatus::Ok);
    const auto& g = ok.instance->frames[0].geometry;
    REQUIRE(g.hasOrientation);
    REQUIRE(std::abs(g.rowDir.dot(g.colDir)) < 1e-12);
    REQUIRE(g.rowDir.norm() == Catch::Approx(1.0));
    REQUIRE(g.colDir.norm() == Catch::Approx(1.0));
    REQUIRE(g.colDir.y == Catch::Approx(1.0).margin(1e-9));
    // Grossly non-orthogonal vectors are not trusted at all.
    img.colDir = {0.2, 0.98, 0.0};
    REQUIRE(writeDicom(dir.path() / "skewed", img));
    const auto bad = parseDicomHeader(dir.path() / "skewed");
    REQUIRE(bad.status == ParseStatus::Ok);
    REQUIRE_FALSE(bad.instance->frames[0].geometry.hasOrientation);
}

TEST_CASE("Implicit VR pixel data that starts like an item tag is not rejected", "[io][security]") {
    // The first pixels happen to be FE FF 00 E0 (an Item tag) followed by a
    // huge "length": the structural check must not treat Pixel Data as a
    // sequence and reject a valid file.
    TempDir dir;
    auto img = ctSlice(0);
    img.pixels[0] = 0xFFFE;
    img.pixels[1] = 0xE000;
    img.pixels[2] = 0xFFFF;
    img.pixels[3] = 0x7FFF;
    REQUIRE(writeDicom(dir.path() / "itemlike", img, Encoding::ImplicitLittle));
    const auto parsed = parseDicomHeader(dir.path() / "itemlike");
    INFO(parsed.message);
    REQUIRE(parsed.status == ParseStatus::Ok);
    const auto dec = decodeInstance(*parsed.instance);
    REQUIRE(dec.ok());
    REQUIRE(dec.frames[0]->rawAt(std::size_t{0}) == -2.0);
}

TEST_CASE("Float Pixel Data is reported as unsupported, not as truncated", "[io][reliability]") {
    TempDir dir;
    REQUIRE(writeDicom(dir.path() / "float", ctSlice(0)));
    REQUIRE(convertToFloatPixelData(dir.path() / "float"));
    const auto parsed = parseDicomHeader(dir.path() / "float");
    REQUIRE(parsed.status == ParseStatus::Unsupported);
    REQUIRE(parsed.message.find("ponto flutuante") != std::string::npos);
}

TEST_CASE("Native YBR_FULL_422 is up-sampled and converted to RGB", "[io][color]") {
    TempDir dir;
    SyntheticImage img;
    img.rows = 2;
    img.columns = 4;
    img.samplesPerPixel = 3;
    img.bitsAllocated = 8;
    img.bitsStored = 8;
    img.highBit = 7;
    img.pixelRepresentation = 0;
    img.writePosition = false;
    img.writeSpacing = false;
    img.modality = "OT";
    img.sopClassUid = "1.2.840.10008.5.1.4.1.1.7";
    img.photometric = "YBR_FULL_422";
    // Each pair of pixels shares Cb/Cr: Y0 Y1 Cb Cr (PS3.3 C.7.6.3.1.2).
    // Pair colours: red, blue, grey, green.
    const int rgb[4][3] = {{255, 0, 0}, {0, 0, 255}, {128, 128, 128}, {0, 255, 0}};
    for (const auto& c : rgb) {
        const double y = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
        const double cb = 128 - 0.168736 * c[0] - 0.331264 * c[1] + 0.5 * c[2];
        const double cr = 128 + 0.5 * c[0] - 0.418688 * c[1] - 0.081312 * c[2];
        for (double v : {y, y, cb, cr}) {
            img.pixels8.push_back(static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)));
        }
    }
    REQUIRE(img.pixels8.size() == 16);  // 8 pixels x 2 bytes
    REQUIRE(writeDicom(dir.path() / "ybr422", img));
    const auto parsed = parseDicomHeader(dir.path() / "ybr422");
    REQUIRE(parsed.status == ParseStatus::Ok);
    REQUIRE_FALSE(parsed.instance->pixelDataTruncated);
    const auto dec = decodeInstance(*parsed.instance);
    REQUIRE(dec.ok());
    const auto& d = dec.frames[0]->data;
    REQUIRE(d.size() == 8 * 3);
    for (int pixel = 0; pixel < 8; ++pixel) {
        for (int c = 0; c < 3; ++c) {
            REQUIRE(std::abs(int(d[static_cast<size_t>(pixel * 3 + c)]) - rgb[pixel / 2][c]) <= 2);
        }
    }
}
