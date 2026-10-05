#pragma once

#include <filesystem>
#include <string>

namespace vtc {

// Paths are stored as UTF-8 std::string everywhere in the model so that
// non-ASCII folder names (e.g. "JOÃO") work identically on Windows, macOS and
// Linux. Convert at the filesystem boundary only.
inline std::string pathToUtf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

inline std::filesystem::path utf8ToPath(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace vtc
