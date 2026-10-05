#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Vec3.h"

namespace vtc {

// Where the in-plane pixel spacing of a frame came from. Measurements in mm
// are only offered when the source is known (section 74: never invent data).
enum class SpacingSource {
    None,               // no calibration: measurements in pixels only
    PixelSpacing,       // (0028,0030) patient-plane calibration
    ImagerPixelSpacing, // (0018,1164) detector-plane calibration (projection X-ray)
    EnhancedPixelMeasures,  // Pixel Measures Functional Group (Enhanced CT/MR)
    UltrasoundRegion    // Sequence of Ultrasound Regions physical deltas
};

// One window (VOI) as stored in the DICOM file.
struct WindowSetting {
    double center = 0.0;
    double width = 1.0;
    std::string explanation;
};

// Tabulated VOI LUT from (0028,3010) VOI LUT Sequence.
struct VoiLutTable {
    int firstMapped = 0;         // first input value mapped
    int bitsPerEntry = 16;
    std::vector<std::uint16_t> data;
    std::string explanation;
};

// Geometry of one 2D frame in patient space.
//
// DICOM conventions (PS3.3 C.7.6.2):
//  - ImagePositionPatient: centre of the first transmitted pixel (row 0, col 0).
//  - ImageOrientationPatient[0..2] (rowDir): direction of increasing COLUMN
//    index, i.e. moving right along a row.
//  - ImageOrientationPatient[3..5] (colDir): direction of increasing ROW index.
//  - PixelSpacing[0] = spacing between adjacent ROWS   -> along colDir (spacingY)
//  - PixelSpacing[1] = spacing between adjacent COLUMNS -> along rowDir (spacingX)
struct FrameGeometry {
    bool hasPosition = false;
    bool hasOrientation = false;
    Vec3 position;
    Vec3 rowDir{1.0, 0.0, 0.0};
    Vec3 colDir{0.0, 1.0, 0.0};
    SpacingSource spacingSource = SpacingSource::None;
    double spacingX = 1.0;  // mm between columns (along rowDir)
    double spacingY = 1.0;  // mm between rows (along colDir)
    std::optional<double> sliceThickness;

    [[nodiscard]] bool hasSpacing() const { return spacingSource != SpacingSource::None; }
    [[nodiscard]] bool isSpatial() const { return hasPosition && hasOrientation && hasSpacing(); }
    [[nodiscard]] Vec3 normal() const { return rowDir.cross(colDir).normalized(); }
    // Patient coordinate of continuous pixel (x = column, y = row).
    [[nodiscard]] Vec3 pixelToPatient(double x, double y) const {
        return position + rowDir * (x * spacingX) + colDir * (y * spacingY);
    }
};

// Attributes that may legitimately split one SeriesInstanceUID into several
// geometric stacks (echoes, phases, acquisitions...).
struct FrameSplitKeys {
    std::optional<int> echoNumber;
    std::optional<double> echoTime;
    std::optional<int> temporalPosition;
    std::optional<int> acquisitionNumber;
    std::optional<double> triggerTime;
    std::optional<double> diffusionBValue;
    std::string stackId;
    std::string imageType;
};

struct FrameInfo {
    int frameIndex = 0;  // 0-based frame number inside the instance
    FrameGeometry geometry;
    double rescaleSlope = 1.0;
    double rescaleIntercept = 0.0;
    std::string rescaleType;  // e.g. "HU"
    std::vector<WindowSetting> windows;
    FrameSplitKeys keys;
};

// Header-level information of one DICOM instance (file). Pixel data is
// decoded lazily by DicomDecoder.
struct InstanceInfo {
    std::string filePath;
    std::uint64_t fileSize = 0;

    std::string sopClassUid;
    std::string sopInstanceUid;
    std::string transferSyntaxUid;
    std::string specificCharacterSet;

    // Patient
    std::string patientName;  // UTF-8, '^' separators replaced for display
    std::string patientId;
    std::string patientBirthDate;
    std::string patientSex;
    std::string patientAge;

    // Study
    std::string studyInstanceUid;
    std::string studyDate;
    std::string studyTime;
    std::string studyDescription;
    std::string accessionNumber;
    std::string institutionName;
    std::string referringPhysician;

    // Series
    std::string seriesInstanceUid;
    std::string seriesDescription;
    std::string modality;
    std::string bodyPartExamined;
    std::string manufacturer;
    std::string manufacturerModel;
    std::string protocolName;
    std::string patientPosition;  // HFS, FFS, HFP...
    std::string frameOfReferenceUid;
    std::optional<int> seriesNumber;
    std::string seriesDate;
    std::string seriesTime;

    // Image
    std::optional<int> instanceNumber;
    std::string acquisitionTime;
    std::string contentTime;
    std::optional<double> spacingBetweenSlices;
    std::optional<double> kvp;
    std::optional<double> recommendedFrameRate;  // frames per second
    std::optional<double> frameTimeMs;

    // Image Pixel module
    int rows = 0;
    int columns = 0;
    int samplesPerPixel = 1;
    int bitsAllocated = 0;
    int bitsStored = 0;
    int highBit = 0;
    int pixelRepresentation = 0;  // 0 unsigned, 1 two's complement
    int planarConfiguration = 0;
    int numberOfFrames = 1;
    std::string photometricInterpretation;
    std::string voiLutFunction;  // LINEAR, LINEAR_EXACT, SIGMOID
    std::optional<VoiLutTable> voiLut;
    bool hasPixelData = false;
    bool isEnhanced = false;
    bool lossyCompressed = false;
    bool hasLocalizerImageType = false;
    std::uint64_t pixelDataOffset = 0;  // file offset of the native Pixel Data value
    bool pixelDataTruncated = false;    // file ends before the declared pixel data

    std::vector<FrameInfo> frames;  // size == numberOfFrames when hasPixelData

    [[nodiscard]] bool isMonochrome() const {
        return photometricInterpretation == "MONOCHROME1" || photometricInterpretation == "MONOCHROME2";
    }
};

using InstancePtr = std::shared_ptr<const InstanceInfo>;

// Reference to one frame of one instance.
struct FrameRef {
    InstancePtr instance;
    int frame = 0;

    [[nodiscard]] const FrameInfo& info() const { return instance->frames.at(static_cast<size_t>(frame)); }
    [[nodiscard]] const FrameGeometry& geometry() const { return info().geometry; }
    // Unique key: file path + frame number.
    [[nodiscard]] std::string key() const { return instance->filePath + "#" + std::to_string(frame); }
};

}  // namespace vtc
