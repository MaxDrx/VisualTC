// make_phantom: writes a synthetic, fully anonymous DICOM exam folder used for
// VisualTC demos, manual QA and automated smoke tests.
//
//   make_phantom <output-dir> [--small]
//
// Contents (patient "SIMULADO^JOÃO", no real data):
//   Study 1 (TC Tórax/Abdome, one Frame of Reference)
//     Series 1  scout (coronal LOCALIZER)          -> reference lines
//     Series 2  axial 1.5 mm, 400x400, HU phantom    -> MPR, ROI, HU
//     Series 3  axial 5 mm, 256x256, JPEG-LS         -> spatial sync
//   Study 2 (TC Crânio, own FoR): axial 1 mm, JPEG 2000 lossless, feet-first numbering
//   Study 3 (RM Crânio, own FoR): sagittal T2-like, 4 mm, two windows, RLE
//   Study 4 (US Abdome): 40-frame cine with calibrated ultrasound region
//   Study 5 (RX Tórax): DX, MONOCHROME1, ImagerPixelSpacing
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "fixtures/DicomFixtures.h"

using vtc::Vec3;
using namespace vtc::test;
namespace fs = std::filesystem;

namespace {

std::mt19937 rng(20260105);

double gauss(double sigma) {
    std::normal_distribution<double> d(0.0, sigma);
    return d(rng);
}

bool inEllipsoid(const Vec3& p, const Vec3& c, const Vec3& r) {
    const double dx = (p.x - c.x) / r.x;
    const double dy = (p.y - c.y) / r.y;
    const double dz = (p.z - c.z) / r.z;
    return dx * dx + dy * dy + dz * dz <= 1.0;
}

double segDist(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    double t = (p - a).dot(ab) / ab.dot(ab);
    t = std::clamp(t, 0.0, 1.0);
    return (p - (a + ab * t)).norm();
}

// Aorta centre line: ascending, arch, descending thoracic and abdominal.
std::vector<Vec3> aortaPath() {
    std::vector<Vec3> path;
    path.push_back({12, -18, 190});
    path.push_back({12, -16, 250});
    for (int i = 0; i <= 10; ++i) {
        const double a = std::numbers::pi * i / 10.0;
        path.push_back({12 - 27 * (1 - std::cos(a)) / 2.0, -16 + 60 * (1 - std::cos(a)) / 2.0, 250 + 32 * std::sin(a)});
    }
    path.push_back({-15, 46, 200});
    path.push_back({-12, 40, 120});
    path.push_back({-8, 30, 40});
    return path;
}
const std::vector<Vec3> kAorta = aortaPath();

// Torso phantom in Hounsfield Units (LPS, mm). z in [0, 340], head up.
double torsoHU(const Vec3& p) {
    const double bx = 165.0;
    const double by = 112.0 + 6.0 * std::sin(p.z / 60.0);
    const double e = (p.x / bx) * (p.x / bx) + (p.y / by) * (p.y / by);
    if (e > 1.0) {
        return -1000.0;  // air
    }
    double hu = 42.0;  // muscle / soft tissue
    const double ef = (p.x / (bx - 13)) * (p.x / (bx - 13)) + (p.y / (by - 12)) * (p.y / (by - 12));
    if (ef > 1.0) {
        hu = -105.0;  // subcutaneous fat
    }
    // Ribs (thorax only)
    if (p.z > 150 && p.z < 322 && ef <= 1.0) {
        const double er = (p.x / (bx - 22)) * (p.x / (bx - 22)) + (p.y / (by - 20)) * (p.y / (by - 20));
        if (er > 0.90 && er <= 1.0 && std::fmod(p.z, 21.0) < 7.0 && p.y < 75) {
            return 680.0;
        }
    }
    // Spine
    const double level = std::fmod(p.z + 1000.0, 30.0);
    const double vb = std::hypot(p.x, p.y - 62);
    if (vb < 19) {
        return level < 24 ? 520.0 + 120.0 * (vb > 16 ? 1 : 0) : 85.0;
    }
    if (std::hypot(p.x, p.y - 86) < 7.5) {
        return 12.0;  // spinal canal
    }
    if (std::pow(p.x / 15.0, 2) + std::pow((p.y - 97) / 7.0, 2) < 1.0 && level < 24) {
        return 600.0;  // posterior elements
    }
    // Aorta (contrast-enhanced)
    for (size_t i = 1; i < kAorta.size(); ++i) {
        const double r = kAorta[i].z > 185 ? 14.0 : 11.0;
        if (segDist(p, kAorta[i - 1], kAorta[i]) < r) {
            return 285.0;
        }
    }
    // Heart with contrast-filled chambers
    if (inEllipsoid(p, {18, -25, 195}, {58, 46, 52})) {
        return inEllipsoid(p, {20, -25, 195}, {36, 28, 32}) ? 240.0 : 48.0;
    }
    // Lungs with vessels
    for (double side : {-1.0, 1.0}) {
        if (p.z > 158 && p.z < 330 && inEllipsoid(p, {side * 78, 5, 248}, {62, 80, 88})) {
            const double v = std::sin(p.x * 0.21) * std::sin(p.y * 0.17) * std::sin(p.z * 0.13);
            if (v > 0.93) {
                return -60.0;  // pulmonary vessels
            }
            if (side < 0 && inEllipsoid(p, {-90, 20, 270}, {7, 7, 7})) {
                return 35.0;  // pulmonary nodule
            }
            return -860.0;
        }
    }
    // Liver (patient right = -x) with hypodense lesion
    if (p.z > 55 && p.z < 180 && inEllipsoid(p, {-68, -8, 125}, {78, 72, 58})) {
        return inEllipsoid(p, {-85, -15, 132}, {13, 13, 13}) ? 18.0 : 62.0;
    }
    if (inEllipsoid(p, {82, 32, 140}, {34, 30, 42})) {
        return 52.0;  // spleen
    }
    for (double side : {-1.0, 1.0}) {
        if (inEllipsoid(p, {side * 62, 56, 92}, {24, 19, 52})) {
            return inEllipsoid(p, {side * 62, 56, 92}, {14, 10, 38}) ? 25.0 : 150.0;  // kidney cortex/medulla
        }
    }
    if (inEllipsoid(p, {58, -42, 162}, {22, 20, 22})) {
        return p.y < -48 ? -920.0 : 15.0;  // stomach air-fluid level
    }
    if (p.z < 80) {
        const double g = std::sin(p.x * 0.09 + 1.0) * std::sin(p.y * 0.11) * std::sin(p.z * 0.07 + 0.5);
        if (g > 0.85 && p.y < 40) {
            return -900.0;  // bowel gas
        }
    }
    return hu;
}

double headHU(const Vec3& p) {
    // Head centred at origin; z up. Radii in mm.
    if (!inEllipsoid(p, {0, 0, 0}, {78, 98, 92})) {
        return -1000.0;
    }
    if (!inEllipsoid(p, {0, 0, 0}, {74, 94, 88})) {
        return 40.0;  // scalp
    }
    if (!inEllipsoid(p, {0, 0, 0}, {67, 87, 81})) {
        return 1150.0;  // skull
    }
    if (inEllipsoid(p, {-28, 18, 22}, {11, 11, 11})) {
        return 72.0;  // small hyperdense lesion
    }
    for (double side : {-1.0, 1.0}) {
        if (inEllipsoid(p, {side * 9, 2, 20}, {6, 24, 14})) {
            return 6.0;  // lateral ventricles
        }
    }
    if (inEllipsoid(p, {0, 0, 10}, {50, 66, 52})) {
        return 29.0;  // white matter
    }
    return 37.0;  // grey matter
}

std::string hexName() {
    static std::uniform_int_distribution<unsigned> d(0, 15);
    std::string s;
    for (int i = 0; i < 10; ++i) {
        s.push_back("0123456789ABCDEF"[d(rng)]);
    }
    return s;
}

SyntheticImage baseCt(const std::string& seriesUid, const std::string& forUid) {
    SyntheticImage img;
    img.patientNameRaw = "SIMULADO^JO\xC3O";  // Latin-1
    img.specificCharacterSet = "ISO_IR 100";
    img.patientId = "VTC-DEMO-001";
    img.seriesInstanceUid = seriesUid;
    img.frameOfReferenceUid = forUid;
    img.bitsAllocated = 16;
    img.bitsStored = 12;
    img.highBit = 11;
    img.pixelRepresentation = 0;
    img.slope = 1.0;
    img.intercept = -1024.0;
    img.patientPosition = "HFS";
    return img;
}

std::uint16_t storeHU(double hu) {
    return static_cast<std::uint16_t>(std::clamp(std::lround(hu + 1024.0), 0L, 4095L));
}

void writeTorsoSeries(const fs::path& dir, const std::string& studyUid, const std::string& forUid, bool small,
                      int seriesNumber, double thickness, int matrix, Encoding enc, const std::string& desc) {
    const double fov = 360.0;
    const double px = fov / matrix;
    const double zTop = 330.0;
    const double zBottom = small ? 180.0 : 0.0;
    const int slices = static_cast<int>(std::floor((zTop - zBottom) / thickness)) + 1;
    const std::string seriesUid = makeUid("ser" + std::to_string(seriesNumber));
    const double x0 = -fov / 2.0 + px / 2.0;
    const int subs = thickness > 2.0 ? 5 : 1;
    for (int k = 0; k < slices; ++k) {
        const double z = zTop - k * thickness;  // head-first: image 1 at the top
        SyntheticImage img = baseCt(seriesUid, forUid);
        img.studyInstanceUid = studyUid;
        img.rows = img.columns = matrix;
        img.seriesNumber = seriesNumber;
        img.seriesDescription = desc;
        img.studyDescription = "TC TORAX E ABDOME";
        img.bodyPart = "CHEST";
        img.instanceNumber = k + 1;
        img.position = {x0, x0, z};
        img.spacingRow = img.spacingColumn = px;
        img.sliceThickness = thickness;
        img.windowCenter = 40;
        img.windowWidth = 400;
        img.extraWindows = {{-600, 1500}};
        img.windowExplanation = "MEDIASTINO\\PULMAO";
        img.pixels.resize(static_cast<size_t>(matrix * matrix));
        for (int j = 0; j < matrix; ++j) {
            for (int i = 0; i < matrix; ++i) {
                double acc = 0.0;
                for (int s = 0; s < subs; ++s) {
                    const double zz = subs == 1 ? z : z - thickness / 2.0 + thickness * (s + 0.5) / subs;
                    acc += torsoHU({x0 + i * px, x0 + j * px, zz});
                }
                double hu = acc / subs;
                if (hu > -990.0) {
                    hu += gauss(subs == 1 ? 9.0 : 4.0);
                }
                img.pixels[static_cast<size_t>(j * matrix + i)] = storeHU(hu);
            }
        }
        if (!writeDicom(dir / hexName(), img, enc)) {
            std::cerr << "falha ao gravar fatia " << k << "\n";
        }
    }
}

void writeScout(const fs::path& dir, const std::string& studyUid, const std::string& forUid) {
    const double px = 0.9;
    const int cols = 400;
    const int rows = 380;
    const double x0 = -180.0 + px / 2.0;
    const double zTop = 340.0;
    SyntheticImage img = baseCt(makeUid("scout"), forUid);
    img.studyInstanceUid = studyUid;
    img.rows = rows;
    img.columns = cols;
    img.seriesNumber = 1;
    img.seriesDescription = "TOPOGRAMA";
    img.studyDescription = "TC TORAX E ABDOME";
    img.imageType = "ORIGINAL\\PRIMARY\\LOCALIZER";
    img.instanceNumber = 1;
    img.position = {x0, 0.0, zTop};
    img.rowDir = {1, 0, 0};
    img.colDir = {0, 0, -1};
    img.spacingRow = px;
    img.spacingColumn = px;
    img.windowCenter = 1200;
    img.windowWidth = 2400;
    img.slope = 1.0;
    img.intercept = 0.0;
    img.pixels.resize(static_cast<size_t>(rows * cols));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            double att = 0.0;
            for (double y = -130; y <= 130; y += 4.0) {
                att += (torsoHU({x0 + c * px, y, zTop - r * px}) + 1000.0) / 1000.0;
            }
            img.pixels[static_cast<size_t>(r * cols + c)] =
                static_cast<std::uint16_t>(std::clamp(att * 38.0 + gauss(6.0), 0.0, 4095.0));
        }
    }
    writeDicom(dir / hexName(), img);
}

void writeHead(const fs::path& dir, bool small) {
    const std::string studyUid = makeUid("headstudy");
    const std::string forUid = makeUid("headfor");
    const std::string seriesUid = makeUid("headser");
    const int matrix = 256;
    const double px = 0.9;
    const int slices = small ? 40 : 110;
    const double x0 = -matrix * px / 2.0 + px / 2.0;
    for (int k = 0; k < slices; ++k) {
        const double z = -70.0 + k * 1.0 * (small ? 2.5 : 1.0);
        SyntheticImage img = baseCt(seriesUid, forUid);
        img.studyInstanceUid = studyUid;
        img.studyDate = "20251110";
        img.studyDescription = "TC CRANIO";
        img.seriesDescription = "CRANIO 1.0 J2K";
        img.bodyPart = "HEAD";
        img.seriesNumber = 2;
        img.rows = img.columns = matrix;
        img.instanceNumber = k + 1;  // feet-first numbering: image 1 is the lowest slice
        img.position = {x0, x0, z};
        img.spacingRow = img.spacingColumn = px;
        img.sliceThickness = small ? 2.5 : 1.0;
        img.windowCenter = 40;
        img.windowWidth = 80;
        img.windowExplanation = "CEREBRO";
        img.pixels.resize(static_cast<size_t>(matrix * matrix));
        for (int j = 0; j < matrix; ++j) {
            for (int i = 0; i < matrix; ++i) {
                double hu = headHU({x0 + i * px, x0 + j * px, z});
                if (hu > -990) {
                    hu += gauss(4.0);
                }
                img.pixels[static_cast<size_t>(j * matrix + i)] = storeHU(hu);
            }
        }
        writeDicom(dir / hexName(), img, Encoding::Jpeg2000Lossless);
    }
}

void writeMr(const fs::path& dir) {
    const std::string studyUid = makeUid("mrstudy");
    const std::string forUid = makeUid("mrfor");
    const std::string seriesUid = makeUid("mrser");
    const int rows = 256;
    const int cols = 256;
    const double px = 0.94;
    for (int k = 0; k < 24; ++k) {
        const double x = -46.0 + k * 4.0;  // sagittal slices along x
        SyntheticImage img;
        img.patientNameRaw = "SIMULADO^JO\xC3\x83O";  // UTF-8
        img.specificCharacterSet = "ISO_IR 192";
        img.patientId = "VTC-DEMO-001";
        img.modality = "MR";
        img.sopClassUid = "1.2.840.10008.5.1.4.1.1.4";
        img.studyInstanceUid = studyUid;
        img.seriesInstanceUid = seriesUid;
        img.frameOfReferenceUid = forUid;
        img.studyDate = "20250820";
        img.studyDescription = "RM CRANIO";
        img.seriesDescription = "SAG T2 FSE";
        img.seriesNumber = 5;
        img.rows = rows;
        img.columns = cols;
        img.bitsAllocated = 16;
        img.bitsStored = 16;
        img.highBit = 15;
        img.pixelRepresentation = 0;
        img.instanceNumber = k + 1;
        img.rowDir = {0, 1, 0};
        img.colDir = {0, 0, -1};
        img.position = {x, -cols * px / 2.0, rows * px / 2.0};
        img.spacingRow = img.spacingColumn = px;
        img.sliceThickness = 3.5;
        img.windowCenter = 700;
        img.windowWidth = 1400;
        img.extraWindows = {{1100, 2200}};
        img.pixels.resize(static_cast<size_t>(rows * cols));
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const Vec3 p{x, -cols * px / 2.0 + c * px, rows * px / 2.0 - r * px};
                const double hu = headHU(p);
                double v = 0.0;  // T2-like contrast: CSF bright, fat/bone dark
                if (hu <= -990) {
                    v = 0;
                } else if (hu > 500) {
                    v = 60;
                } else if (hu < 15) {
                    v = 1900;
                } else if (hu < 33) {
                    v = 650;
                } else if (hu < 50) {
                    v = 850;
                } else {
                    v = 1200;
                }
                v = std::max(0.0, v + gauss(25.0));
                img.pixels[static_cast<size_t>(r * cols + c)] = static_cast<std::uint16_t>(v);
            }
        }
        writeDicom(dir / hexName(), img, Encoding::RleLossless);
    }
}

void writeUs(const fs::path& dir) {
    const int rows = 360;
    const int cols = 480;
    const int frames = 40;
    SyntheticImage img;
    img.patientNameRaw = "SIMULADO^JO\xC3O";
    img.specificCharacterSet = "ISO_IR 100";
    img.patientId = "VTC-DEMO-001";
    img.modality = "US";
    img.sopClassUid = "1.2.840.10008.5.1.4.1.1.3.1";  // US Multi-frame
    img.studyInstanceUid = makeUid("usstudy");
    img.seriesInstanceUid = makeUid("usser");
    img.frameOfReferenceUid.clear();
    img.studyDate = "20260201";
    img.studyDescription = "US ABDOME TOTAL";
    img.seriesDescription = "FIGADO CINE";
    img.seriesNumber = 1;
    img.rows = rows;
    img.columns = cols;
    img.bitsAllocated = 8;
    img.bitsStored = 8;
    img.highBit = 7;
    img.pixelRepresentation = 0;
    img.numberOfFrames = frames;
    img.frameTimeMs = 40.0;
    img.writePosition = false;
    img.writeSpacing = false;
    img.usPhysicalDeltaCm = 0.03;  // 0.3 mm per pixel
    img.imageType = "ORIGINAL\\PRIMARY";
    img.instanceNumber = 1;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> speckle(static_cast<size_t>(rows * cols));
    for (auto& s : speckle) {
        s = u(rng);
    }
    for (int f = 0; f < frames; ++f) {
        const double phase = 2.0 * std::numbers::pi * f / frames;
        const double cystX = 250 + 6 * std::sin(phase);
        const double cystY = 190 + 4 * std::cos(phase);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                // Sector (fan) geometry from the transducer at the top centre.
                const double dx = c - cols / 2.0;
                const double dy = r + 40.0;
                const double ang = std::atan2(dx, dy);
                const double dist = std::hypot(dx, dy);
                double v = 0.0;
                if (std::abs(ang) < 0.62 && dist > 50 && dist < 395) {
                    const double depthGain = 1.0 - (dist - 50) / 600.0;
                    double tissue = 95 + 60 * speckle[static_cast<size_t>(r * cols + c)];
                    if (std::hypot(c - cystX, r - cystY) < 34) {
                        tissue = 8 + 10 * speckle[static_cast<size_t>(r * cols + c)];  // anechoic cyst
                        if (r > cystY + 30) {
                            tissue = 0;
                        }
                    } else if (std::hypot(c - cystX, r - cystY) < 38 && r > cystY) {
                        tissue = 210;  // posterior wall
                    }
                    if (r > 290 && r < 300) {
                        tissue = 200;  // diaphragm line
                    }
                    v = tissue * depthGain;
                }
                img.pixels8.push_back(static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0)));
            }
        }
    }
    writeDicom(dir / hexName(), img);
}

void writeDx(const fs::path& dir) {
    const int n = 1024;
    const double px = 0.36;
    SyntheticImage img;
    img.patientNameRaw = "SIMULADO^JO\xC3O";
    img.specificCharacterSet = "ISO_IR 100";
    img.patientId = "VTC-DEMO-001";
    img.modality = "DX";
    img.sopClassUid = "1.2.840.10008.5.1.4.1.1.1.1";
    img.studyInstanceUid = makeUid("dxstudy");
    img.seriesInstanceUid = makeUid("dxser");
    img.frameOfReferenceUid.clear();
    img.studyDate = "20251215";
    img.studyDescription = "RX TORAX PA";
    img.seriesDescription = "PA";
    img.seriesNumber = 1;
    img.rows = img.columns = n;
    img.bitsAllocated = 16;
    img.bitsStored = 12;
    img.highBit = 11;
    img.pixelRepresentation = 0;
    img.photometric = "MONOCHROME1";
    img.writePosition = false;
    img.spacingIsImager = true;
    img.spacingRow = img.spacingColumn = px;
    img.windowCenter = 2000;
    img.windowWidth = 3000;
    img.imageType = "ORIGINAL\\PRIMARY";
    img.instanceNumber = 1;
    img.pixels.resize(static_cast<size_t>(n * n));
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            const double x = (c - n / 2.0) * px;
            const double z = 330.0 - r * px;
            double att = 0.0;
            for (double y = -120; y <= 120; y += 8.0) {
                att += (torsoHU({x, y, z}) + 1000.0) / 1000.0;
            }
            // MONOCHROME1: low stored values are displayed white (bone).
            const double stored = 4095.0 - std::clamp(att * 120.0, 0.0, 4095.0) + gauss(10.0);
            img.pixels[static_cast<size_t>(r * n + c)] = static_cast<std::uint16_t>(std::clamp(stored, 0.0, 4095.0));
        }
    }
    writeDicom(dir / hexName(), img);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "uso: make_phantom <pasta-de-saida> [--small]\n";
        return 1;
    }
    const fs::path out = argv[1];
    const bool small = argc > 2 && std::string(argv[2]) == "--small";
    fs::create_directories(out / "DICOM" / "ST000001" / "SE000001");
    fs::create_directories(out / "DICOM" / "ST000001" / "SE000002");
    fs::create_directories(out / "DICOM" / "ST000001" / "SE000003");
    fs::create_directories(out / "DICOM" / "ST000002");
    fs::create_directories(out / "DICOM" / "ST000003");
    fs::create_directories(out / "DICOM" / "ST000004");
    fs::create_directories(out / "DICOM" / "ST000005");

    const std::string studyUid = makeUid("torso");
    const std::string forUid = makeUid("torsofor");
    std::cout << "Topograma...\n";
    writeScout(out / "DICOM" / "ST000001" / "SE000001", studyUid, forUid);
    std::cout << "TC tórax/abdome 1,5 mm...\n";
    writeTorsoSeries(out / "DICOM" / "ST000001" / "SE000002", studyUid, forUid, small, 2, 1.5, small ? 256 : 400,
                     Encoding::ExplicitLittle, "AXIAL 1.5 MM");
    std::cout << "TC tórax/abdome 5 mm (JPEG-LS)...\n";
    writeTorsoSeries(out / "DICOM" / "ST000001" / "SE000003", studyUid, forUid, small, 3, 5.0, 256,
                     Encoding::JpegLsLossless, "AXIAL 5 MM");
    std::cout << "TC crânio (JPEG 2000)...\n";
    writeHead(out / "DICOM" / "ST000002", small);
    std::cout << "RM crânio (RLE)...\n";
    writeMr(out / "DICOM" / "ST000003");
    std::cout << "US cine...\n";
    writeUs(out / "DICOM" / "ST000004");
    std::cout << "RX tórax...\n";
    writeDx(out / "DICOM" / "ST000005");
    std::cout << "Concluído: " << out << "\n";
    return 0;
}
