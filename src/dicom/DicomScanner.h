#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "dicom/DicomParser.h"
#include "dicom/DicomTypes.h"

namespace vtc {

using HeaderParser = std::function<ParseResult(const std::filesystem::path&)>;

struct ScanOptions {
    ParseLimits limits;
    HeaderParser parser;  // empty = parseDicomHeader in-process
    std::size_t maxFiles = 500000;  // stop walking after this many files
    unsigned threads = 0;           // 0 = automatic
};

struct ScanIssue {
    ParseStatus status = ParseStatus::Malformed;
    std::string filePath;  // shown only in the UI, never logged above Debug
    std::string message;
};

struct ScanProgress {
    std::size_t filesFound = 0;     // files discovered while walking
    std::size_t filesProcessed = 0;
    std::size_t dicomImages = 0;
};

struct ScanResult {
    std::vector<InstancePtr> instances;   // displayable images
    std::vector<ScanIssue> issues;        // malformed / unsupported files
    std::size_t filesVisited = 0;
    std::size_t nonDicomFiles = 0;
    std::size_t nonImageObjects = 0;
    bool cancelled = false;
    bool truncated = false;  // maxFiles reached
};

using ScanProgressCallback = std::function<void(const ScanProgress&)>;

// Recursively finds DICOM files (independently of name or extension) under
// the given files/folders and reads their headers in parallel.
class DicomScanner {
public:
    explicit DicomScanner(ScanOptions options = {}) : options_(std::move(options)) {}

    ScanResult scan(const std::vector<std::filesystem::path>& inputs, const ScanProgressCallback& progress = {},
                    const std::atomic<bool>* cancel = nullptr) const;

    // Lists candidate files (regular files, no directory symlink loops).
    std::vector<std::filesystem::path> collectFiles(const std::vector<std::filesystem::path>& inputs,
                                                    bool* truncated = nullptr) const;

private:
    ScanOptions options_;
};

}  // namespace vtc
