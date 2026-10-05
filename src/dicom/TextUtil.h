#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vtc {

// Converts a DICOM string value to UTF-8 according to (0008,0005)
// SpecificCharacterSet. Supported: default repertoire, ISO_IR 100 (Latin-1),
// ISO_IR 192 (UTF-8). Unknown single-byte sets fall back to Latin-1 so that
// accented Portuguese names never become invalid UTF-8.
std::string dicomToUtf8(std::string_view raw, std::string_view specificCharacterSet);

bool isValidUtf8(std::string_view s);

// Trims spaces and NUL padding (DICOM pads to even length).
std::string trimDicom(std::string_view s);

// "SILVA^JOAO^^DR" -> "SILVA JOAO". Only the alphabetic group (before '=').
std::string formatPersonName(std::string_view pn);

// Splits a multi-valued string on '\\'.
std::vector<std::string> splitBackslash(std::string_view s);

// Parses DS/IS values robustly. Returns nullopt for empty or invalid text and
// for non-finite numbers.
std::optional<double> parseDouble(std::string_view s);
std::optional<int> parseInt(std::string_view s);
std::vector<double> parseDoubles(std::string_view multi);

// "20240131" -> "31/01/2024" (returns input unchanged when malformed).
std::string formatDicomDate(std::string_view da);
// "143015.123" -> "14:30:15"
std::string formatDicomTime(std::string_view tm);

}  // namespace vtc
