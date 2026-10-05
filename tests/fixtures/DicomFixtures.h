#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/Vec3.h"

namespace vtc::test {

// Description of one synthetic single-frame DICOM image written by GDCM.
struct SyntheticImage {
    int rows = 16;
    int columns = 16;
    int bitsAllocated = 16;
    int bitsStored = 16;
    int highBit = 15;
    int pixelRepresentation = 1;  // signed
    std::string photometric = "MONOCHROME2";
    std::vector<std::uint16_t> pixels;  // raw 16-bit words as written (rows*cols)
    std::vector<std::uint8_t> pixels8;  // used when bitsAllocated == 8 (rows*cols*samples)
    int samplesPerPixel = 1;

    std::string sopClassUid = "1.2.840.10008.5.1.4.1.1.2";  // CT Image Storage
    std::string sopInstanceUid;
    std::string studyInstanceUid = "1.2.826.0.1.3680043.10.1.1";
    std::string seriesInstanceUid = "1.2.826.0.1.3680043.10.1.1.1";
    std::string frameOfReferenceUid = "1.2.826.0.1.3680043.10.1.1.9";
    std::string modality = "CT";
    std::string patientNameRaw = "TESTE^PACIENTE";  // bytes as stored
    std::string specificCharacterSet;               // e.g. "ISO_IR 100"
    std::string patientId = "VTC001";
    std::string seriesDescription = "AXIAL";
    std::optional<int> instanceNumber;
    std::optional<int> echoNumber;

    bool writePosition = true;
    vtc::Vec3 position;
    vtc::Vec3 rowDir{1, 0, 0};
    vtc::Vec3 colDir{0, 1, 0};
    bool writeSpacing = true;
    double spacingRow = 0.5;     // PixelSpacing[0] (between rows)
    double spacingColumn = 0.5;  // PixelSpacing[1] (between columns)
    std::optional<double> sliceThickness;
    std::optional<double> slope;
    std::optional<double> intercept;
    std::optional<double> windowCenter;
    std::optional<double> windowWidth;
    std::string windowExplanation;
    std::vector<std::pair<double, double>> extraWindows;  // (center, width)

    int numberOfFrames = 1;                     // pixels/pixels8 hold all frames
    std::optional<double> usPhysicalDeltaCm;    // Sequence of Ultrasound Regions
    bool spacingIsImager = false;               // write ImagerPixelSpacing instead
    std::string imageType = "ORIGINAL\\PRIMARY\\AXIAL";
    std::string studyDescription = "TC TORAX";
    std::string studyDate = "20260105";
    std::string bodyPart;
    int seriesNumber = 3;
    std::optional<double> frameTimeMs;
    std::optional<double> spacingBetweenSlices;
    std::string patientPosition;
    // PALETTE COLOR lookup tables (8-bit entries, 256 each) when non-empty.
    std::vector<std::uint16_t> paletteRed;
    std::vector<std::uint16_t> paletteGreen;
    std::vector<std::uint16_t> paletteBlue;
};

enum class Encoding {
    ExplicitLittle,
    ImplicitLittle,
    JpegLossless,     // 1.2.840.10008.1.2.4.70
    JpegLsLossless,   // 1.2.840.10008.1.2.4.80
    Jpeg2000Lossless, // 1.2.840.10008.1.2.4.90
    RleLossless,      // 1.2.840.10008.1.2.5
    Deflated,         // 1.2.840.10008.1.2.1.99
    ExplicitBig,      // 1.2.840.10008.1.2.2 (retired)
    JpegBaseline      // 1.2.840.10008.1.2.4.50 (lossy, 8-bit)
};

// Writes `img` to `file`. Returns false on failure.
bool writeDicom(const std::filesystem::path& file, const SyntheticImage& img, Encoding encoding = Encoding::ExplicitLittle);

// Enhanced CT multi-frame object with Shared and Per-frame Functional Groups.
// Frame k is at `origins[k]`; stored = hu + 1024 with intercept -1024.
struct EnhancedSpec {
    int rows = 16;
    int columns = 16;
    double spacing = 0.75;
    std::vector<vtc::Vec3> origins;
    std::vector<std::vector<std::int16_t>> hu;  // one vector per frame
    double windowCenter = 40.0;
    double windowWidth = 400.0;
};
bool writeEnhancedCt(const std::filesystem::path& file, const EnhancedSpec& spec);

// Re-encodes an existing DICOM file with GDCM into another transfer syntax.
bool transcode(const std::filesystem::path& in, const std::filesystem::path& out, Encoding encoding);

// Rewrites only the Photometric Interpretation (0028,0004) of an existing
// file, leaving the (possibly encapsulated) pixel data untouched.
bool setPhotometric(const std::filesystem::path& file, const std::string& photometric);

// Rewrites one binary US attribute (e.g. Planar Configuration) of an existing
// file, leaving the pixel data untouched.
bool setUS(const std::filesystem::path& file, std::uint16_t group, std::uint16_t element, std::uint16_t value);

// Replaces the integer Pixel Data (7FE0,0010) by Float Pixel Data
// (7FE0,0008, VR OF) with the same number of samples (parametric maps).
bool convertToFloatPixelData(const std::filesystem::path& file);

// Strips the 128-byte preamble, "DICM" and the File Meta group, producing a
// raw implicit-VR dataset as written by some old modalities.
bool stripPart10Header(const std::filesystem::path& in, const std::filesystem::path& out);

std::string makeUid(const std::string& suffix);

// Fresh temporary directory, removed by the destructor.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace vtc::test
