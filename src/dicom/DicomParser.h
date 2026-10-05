#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "dicom/DicomTypes.h"

namespace vtc {

// Hard limits applied before any allocation, so that malformed or hostile
// files cannot trigger huge allocations or integer overflows (section 55).
struct ParseLimits {
    int maxRows = 32768;
    int maxColumns = 32768;
    int maxFrames = 20000;
    std::uint64_t maxPixelBytes = 4ull * 1024ull * 1024ull * 1024ull;  // decoded size
    std::uint64_t maxFileBytes = 8ull * 1024ull * 1024ull * 1024ull;
};

enum class ParseStatus {
    Ok,
    NotDicom,      // not a DICOM file (silently ignored during scanning)
    NonImage,      // valid DICOM without displayable pixels (SR, PR, DICOMDIR...)
    Malformed,     // DICOM-like but unreadable / inconsistent
    Unsupported    // readable but outside what VisualTC can display
};

struct ParseResult {
    ParseStatus status = ParseStatus::NotDicom;
    std::shared_ptr<InstanceInfo> instance;
    std::string message;  // user-facing reason (pt-BR), never contains PHI
};

// Cheap content sniffing: "DICM" magic at offset 128, or a plausible raw
// little-endian dataset start (group 0x0002/0x0008). Does not trust extension.
bool looksLikeDicom(const std::filesystem::path& file);
// Same test on the first bytes of a file (at least 132 for the Part 10 case).
bool looksLikeDicomBytes(const unsigned char* data, std::size_t size);

// Reads all header attributes up to (but excluding) Pixel Data.
ParseResult parseDicomHeader(const std::filesystem::path& file, const ParseLimits& limits = {});

// Human readable name of a transfer syntax UID (for messages / DICOM_SUPPORT).
std::string transferSyntaxName(const std::string& uid);
std::string gdcmVersion();
// Initializes the DICOM library globals on the calling (main) thread and
// silences its console diagnostics. Call once at startup.
void initializeDicomLibrary();
bool isTransferSyntaxSupported(const std::string& uid);

}  // namespace vtc
