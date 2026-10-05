#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vtc {

// Compressed/container files the viewer can open directly (exams sent by
// e-mail or downloaded from portals, CD images). Detected by CONTENT, never
// by file extension.
enum class ArchiveKind { None, Zip, SevenZip, Rar, Tar, Gzip, Bzip2, Xz, Zstd, Iso9660 };

ArchiveKind detectArchive(const std::filesystem::path& file);
// Same test on the first bytes of a file; `size` is the total file size
// (needed for the ISO 9660 signature at offset 32769).
ArchiveKind detectArchiveBytes(const unsigned char* data, std::size_t n);
std::string archiveKindName(ArchiveKind kind);

struct ExtractLimits {
    std::uint64_t maxTotalBytes = 64ull << 30;  // everything written for one archive (and its nested ones)
    std::uint64_t maxEntryBytes = 8ull << 30;   // one member (same as ParseLimits::maxFileBytes)
    std::uint64_t maxEntries = 500'000;         // members examined
    std::uint64_t minFreeBytes = 1ull << 30;    // never fill the disk: keep this much free
    // Decompression-bomb guard: above `ratioCheckAfter` written bytes, the
    // output may not exceed `maxRatio` times the archive size.
    std::uint64_t ratioCheckAfter = 1ull << 30;
    double maxRatio = 200.0;
    int maxDepth = 3;  // archives inside archives
};

enum class ExtractStatus {
    Ok,
    NeedsPassword,   // encrypted ZIP and no password given
    WrongPassword,   // encrypted ZIP and the password does not decrypt it
    Unsupported,     // encryption or format variant libarchive cannot read (7z/RAR with password...)
    Corrupt,         // damaged or truncated archive
    LimitExceeded,   // decompression bomb / size limits
    DiskFull,        // not enough free space for the extracted images
    IoError,         // cannot write the extraction folder
    Cancelled
};

struct ExtractedFile {
    std::string path;         // UTF-8 path of the extracted copy (inside the destination folder)
    std::string displayName;  // e.g. "exame.zip › DICOM/IM0001" (for messages, never logged)
};

struct ExtractResult {
    ExtractStatus status = ExtractStatus::Ok;
    std::string message;  // pt-BR, for the user
    std::vector<ExtractedFile> files;
    std::uint64_t entries = 0;          // members examined
    std::uint64_t skippedNonDicom = 0;  // members that are neither DICOM nor archives
    std::uint64_t bytesWritten = 0;
    [[nodiscard]] bool ok() const { return status == ExtractStatus::Ok; }
};

// Extracts the DICOM members of `archive` into `destDir` (created if
// needed), recursing into nested archives.
//
// Safety: member names are NEVER used as file system paths. Every member is
// written as a numbered file directly inside `destDir`, so absolute paths,
// "../" components, links and device entries cannot escape it; links and
// special entries are skipped. Only members whose content looks like DICOM
// (or like a nested archive) are written. Sizes, counts, free disk space and
// the compression ratio are bounded by `limits`. The original archive is only
// read. Thread-compatible; meant to run inside the isolated worker process.
ExtractResult extractArchive(const std::filesystem::path& archive, const std::filesystem::path& destDir,
                             const std::string& displayPrefix, const std::string& password = {},
                             const ExtractLimits& limits = {}, const std::atomic<bool>* cancel = nullptr);

std::string libarchiveVersion();

}  // namespace vtc
