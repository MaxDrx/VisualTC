#pragma once

#include <filesystem>
#include <string>

namespace vtc {

// Structural pre-check of a DICOM file before it is handed to GDCM.
//
// GDCM allocates element buffers from the declared value length *before*
// reading them, so a single corrupted length field (e.g. 0xFFFFFFF0) makes it
// try to allocate gigabytes. This walker visits every element header
// (including nested sequence items) and rejects files whose declared lengths
// exceed the bytes that actually remain in the file. It never allocates
// proportionally to declared lengths, bounds recursion depth and element
// count, and only rejects on proven inconsistencies: constructs it does not
// understand end the walk without rejecting the file.
enum class PreflightResult { Ok, Corrupt };

PreflightResult preflightDicom(const std::filesystem::path& file, std::string* reason = nullptr);

}  // namespace vtc
