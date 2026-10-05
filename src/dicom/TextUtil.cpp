#include "dicom/TextUtil.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace vtc {

namespace {
std::string latin1ToUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }
}  // namespace

bool isValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c >> 5) == 0x6) {
            len = 2;
        } else if ((c >> 4) == 0xE) {
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > s.size()) {
            return false;
        }
        for (size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

std::string dicomToUtf8(std::string_view raw, std::string_view charset) {
    if (contains(charset, "ISO_IR 192")) {
        return isValidUtf8(raw) ? std::string(raw) : latin1ToUtf8(raw);
    }
    bool ascii = true;
    for (unsigned char c : raw) {
        if (c >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii) {
        return std::string(raw);
    }
    if (charset.empty() && isValidUtf8(raw)) {
        // Non-conformant but common: UTF-8 without declaring ISO_IR 192.
        return std::string(raw);
    }
    // ISO_IR 100 and unknown single-byte character sets.
    return latin1ToUtf8(raw);
}

std::string trimDicom(std::string_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\0')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\0')) {
        --e;
    }
    return std::string(s.substr(b, e - b));
}

std::string formatPersonName(std::string_view pn) {
    const auto eq = pn.find('=');
    const std::string_view alpha = pn.substr(0, eq);
    std::string out;
    out.reserve(alpha.size());
    bool pendingSpace = false;
    for (char c : alpha) {
        if (c == '^' || c == ' ') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> splitBackslash(std::string_view s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find('\\', start);
        parts.push_back(trimDicom(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start)));
        if (pos == std::string_view::npos) {
            break;
        }
        start = pos + 1;
    }
    return parts;
}

std::optional<double> parseDouble(std::string_view s) {
    const std::string t = trimDicom(s);
    if (t.empty()) {
        return std::nullopt;
    }
    // std::from_chars for double is not available on every toolchain we
    // target (Apple Clang < 16), so use strtod on a NUL-terminated copy.
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) {
        return std::nullopt;
    }
    while (*end == ' ') {
        ++end;
    }
    if (*end != '\0') {
        return std::nullopt;
    }
    return v;
}

std::optional<int> parseInt(std::string_view s) {
    const std::string t = trimDicom(s);
    if (t.empty()) {
        return std::nullopt;
    }
    const char* b = t.c_str();
    if (*b == '+') {
        ++b;
    }
    int v = 0;
    const auto res = std::from_chars(b, t.c_str() + t.size(), v);
    if (res.ec != std::errc{} || res.ptr != t.c_str() + t.size()) {
        // Some writers store IS values like "12.0".
        if (auto d = parseDouble(t); d && std::abs(*d) < 2.0e9 && std::floor(*d) == *d) {
            return static_cast<int>(*d);
        }
        return std::nullopt;
    }
    return v;
}

std::vector<double> parseDoubles(std::string_view multi) {
    std::vector<double> out;
    for (const auto& p : splitBackslash(multi)) {
        if (auto v = parseDouble(p)) {
            out.push_back(*v);
        } else {
            return {};  // all-or-nothing: partial vectors are not trustworthy
        }
    }
    return out;
}

std::string formatDicomDate(std::string_view da) {
    const std::string t = trimDicom(da);
    if (t.size() == 8) {
        for (char c : t) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return t;
            }
        }
        return t.substr(6, 2) + "/" + t.substr(4, 2) + "/" + t.substr(0, 4);
    }
    return t;
}

std::string formatDicomTime(std::string_view tm) {
    const std::string t = trimDicom(tm);
    if (t.size() >= 4 && std::isdigit(static_cast<unsigned char>(t[0]))) {
        std::string out = t.substr(0, 2) + ":" + t.substr(2, 2);
        if (t.size() >= 6) {
            out += ":" + t.substr(4, 2);
        }
        return out;
    }
    return t;
}

}  // namespace vtc
