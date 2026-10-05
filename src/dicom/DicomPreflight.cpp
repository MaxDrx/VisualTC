#include "dicom/DicomPreflight.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>

namespace vtc {

namespace {

constexpr std::uint32_t kUndefined = 0xFFFFFFFFu;
constexpr int kMaxDepth = 32;
constexpr long kMaxElements = 5'000'000;

enum class Walk { Ok, Corrupt, Stop };  // Stop: not understood, accept file

bool isKnownVr(const char* vr) {
    static const std::array<const char*, 34> known{"AE", "AS", "AT", "CS", "DA", "DS", "DT", "FD", "FL",
                                                    "IS", "LO", "LT", "OB", "OD", "OF", "OL", "OV", "OW",
                                                    "PN", "SH", "SL", "SQ", "SS", "ST", "SV", "TM", "UC",
                                                    "UI", "UL", "UN", "UR", "US", "UT", "UV"};
    for (const char* v : known) {
        if (vr[0] == v[0] && vr[1] == v[1]) {
            return true;
        }
    }
    return false;
}

bool isLongVr(const char* vr) {
    static const std::array<const char*, 13> longVrs{"OB", "OD", "OF", "OL", "OV", "OW", "SQ",
                                                      "SV", "UC", "UN", "UR", "UT", "UV"};
    for (const char* v : longVrs) {
        if (vr[0] == v[0] && vr[1] == v[1]) {
            return true;
        }
    }
    return false;
}

// Attributes that are never sequences and hold arbitrary bytes.
bool isBinaryData(std::uint16_t group, std::uint16_t element) {
    if (group == 0x7FE0) {
        return true;  // (Float/Double Float) Pixel Data
    }
    if (group >= 0x6000 && group <= 0x60FF && (group % 2) == 0 && element == 0x3000) {
        return true;  // Overlay Data
    }
    if (group == 0x0028) {
        // Palette / segmented palette / VOI / modality LUT data
        return (element >= 0x1201 && element <= 0x1204) || (element >= 0x1221 && element <= 0x1223) ||
               element == 0x3006 || element == 0x1101 || element == 0x1102 || element == 0x1103;
    }
    return group == 0x5400 && element == 0x1010;  // Waveform Data
}

class Walker {
public:
    Walker(std::ifstream& in, std::uint64_t size) : in_(in), size_(size) {}

    std::string reason;

    // RLE Lossless: every fragment after the Basic Offset Table must start
    // with a sane 64-byte RLE header (PS3.5 Annex G). GDCM trusts it blindly
    // (division by zero for 0 segments, reads past its 15-entry offset
    // table for more), and it decodes RLE already while reading the header.
    void setRle(bool rle) { rle_ = rle; }

    std::uint64_t pos() const { return pos_; }
    bool seek(std::uint64_t p) {
        if (p > size_) {
            return false;
        }
        pos_ = p;
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(p));
        return static_cast<bool>(in_);
    }
    bool read(void* dst, std::size_t n) {
        if (pos_ + n > size_) {
            return false;
        }
        in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        if (!in_) {
            return false;
        }
        pos_ += n;
        return true;
    }
    bool u16(std::uint16_t& v, bool be) {
        unsigned char b[2];
        if (!read(b, 2)) {
            return false;
        }
        v = be ? static_cast<std::uint16_t>((b[0] << 8) | b[1]) : static_cast<std::uint16_t>((b[1] << 8) | b[0]);
        return true;
    }
    bool u32(std::uint32_t& v, bool be) {
        unsigned char b[4];
        if (!read(b, 4)) {
            return false;
        }
        v = be ? (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3]
               : (std::uint32_t(b[3]) << 24) | (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[1]) << 8) | b[0];
        return true;
    }
    bool peekItemTag(bool be) {
        const std::uint64_t save = pos_;
        std::uint16_t g = 0;
        std::uint16_t e = 0;
        const bool ok = u16(g, be) && u16(e, be);
        seek(save);
        return ok && g == 0xFFFE && e == 0xE000;
    }

    // Called with pos_ at the (invalid) VR field. A lenient reader may treat
    // the next bytes as a 16-bit length, a 32-bit length after 2 reserved
    // bytes, or an implicit 32-bit length: each must fit in the file.
    Walk invalidVr(bool be) {
        const std::uint64_t at = pos_;
        std::uint32_t implicitLen = 0;
        std::uint16_t shortLen = 0;
        std::uint32_t longLen = 0;
        bool bad = false;
        if (seek(at) && u32(implicitLen, be) && implicitLen != kUndefined && implicitLen > size_ - pos_) {
            bad = true;
        }
        if (seek(at + 2) && u16(shortLen, be) && shortLen > size_ - pos_) {
            bad = true;
        }
        if (seek(at + 4) && u32(longLen, be) && longLen != kUndefined && longLen > size_ - pos_) {
            bad = true;
        }
        return bad ? corrupt("VR inválida com comprimento incompatível") : Walk::Stop;
    }

    Walk corrupt(const char* why) {
        reason = why;
        return Walk::Corrupt;
    }

    // Walks data elements until `end`, or until an Item Delimitation Item
    // when `untilItemDelimiter` is set.
    Walk dataset(std::uint64_t end, bool explicitVr, bool be, int depth, bool untilItemDelimiter) {
        if (depth > kMaxDepth) {
            return Walk::Stop;
        }
        while (pos_ + 8 <= end) {
            if (++elements_ > kMaxElements) {
                return Walk::Stop;
            }
            std::uint16_t group = 0;
            std::uint16_t element = 0;
            if (!u16(group, be) || !u16(element, be)) {
                return Walk::Ok;
            }
            if (group == 0xFFFE) {
                std::uint32_t len = 0;
                if (!u32(len, be)) {
                    return Walk::Ok;
                }
                if (len != 0 && len != kUndefined && len > size_ - pos_) {
                    return corrupt("delimitador com comprimento inválido");
                }
                if (element == 0xE00D && untilItemDelimiter) {
                    return len == 0 ? Walk::Ok : corrupt("delimitador de item com comprimento inválido");
                }
                return Walk::Stop;
            }
            bool sq = false;
            bool undefinedAllowed = !explicitVr;
            bool implicitContent = !explicitVr;
            bool pixelData = group == 0x7FE0 && element == 0x0010;
            std::uint32_t len = 0;
            if (explicitVr) {
                char vr[2];
                if (!read(vr, 2)) {
                    return Walk::Ok;
                }
                if (!isKnownVr(vr)) {
                    seek(pos_ - 2);
                    return invalidVr(be);
                }
                if (isLongVr(vr)) {
                    std::uint16_t reserved = 0;
                    if (!u16(reserved, be) || !u32(len, be)) {
                        return Walk::Ok;
                    }
                } else {
                    std::uint16_t l = 0;
                    if (!u16(l, be)) {
                        return Walk::Ok;
                    }
                    len = l;
                }
                sq = vr[0] == 'S' && vr[1] == 'Q';
                const bool un = vr[0] == 'U' && vr[1] == 'N';
                const bool ob = vr[0] == 'O' && (vr[1] == 'B' || vr[1] == 'W');
                undefinedAllowed = sq || un || ob;
                implicitContent = un;  // UN with undefined length is implicit VR LE
            } else if (!u32(len, be)) {
                return Walk::Ok;
            }

            if (len == kUndefined) {
                if (!undefinedAllowed) {
                    return corrupt("comprimento indefinido em elemento que não o permite");
                }
                const Walk w = sequenceUndefined(end, explicitVr && !implicitContent, be, depth + 1, pixelData);
                if (w != Walk::Ok) {
                    return w;
                }
                continue;
            }
            if (len > end - pos_) {
                return corrupt("comprimento de elemento maior que o arquivo");
            }
            const std::uint64_t valueEnd = pos_ + len;
            // Implicit VR has no VR field: a value that starts with an Item
            // tag is probably a sequence. Never apply that guess to binary
            // data whose first bytes are arbitrary (pixels, overlays, LUTs).
            if (sq || (!explicitVr && len >= 8 && !isBinaryData(group, element) && peekItemTag(be))) {
                const Walk w = sequenceDefined(valueEnd, explicitVr, be, depth + 1);
                if (w == Walk::Corrupt) {
                    return w;
                }
            }
            if (!seek(valueEnd)) {
                return Walk::Ok;
            }
        }
        return Walk::Ok;
    }

    // Called with pos_ at the start of an RLE fragment of `len` bytes.
    Walk rleHeader(std::uint32_t len) {
        if (len < 64) {
            return corrupt("fragmento RLE menor que o cabeçalho");
        }
        std::uint32_t header[16] = {};
        for (auto& v : header) {
            if (!u32(v, false)) {
                return Walk::Ok;  // truncation is reported elsewhere
            }
        }
        const std::uint32_t segments = header[0];
        if (segments < 1 || segments > 15) {
            return corrupt("cabeçalho RLE com número de segmentos inválido");
        }
        if (header[1] != 64) {
            return corrupt("cabeçalho RLE com deslocamento inicial inválido");
        }
        for (std::uint32_t i = 2; i <= segments; ++i) {
            if (header[i] <= header[i - 1] || header[i] >= len) {
                return corrupt("cabeçalho RLE com deslocamentos inválidos");
            }
        }
        return Walk::Ok;
    }

    Walk sequenceUndefined(std::uint64_t end, bool explicitVr, bool be, int depth, bool fragments) {
        if (depth > kMaxDepth) {
            return Walk::Stop;
        }
        int fragmentIndex = 0;
        while (pos_ + 8 <= end) {
            if (++elements_ > kMaxElements) {
                return Walk::Stop;
            }
            std::uint16_t g = 0;
            std::uint16_t e = 0;
            std::uint32_t len = 0;
            if (!u16(g, be) || !u16(e, be) || !u32(len, be)) {
                return Walk::Ok;
            }
            if (g == 0xFFFE && e == 0xE0DD) {
                return len == 0 ? Walk::Ok : corrupt("delimitador de sequência com comprimento inválido");
            }
            if (g != 0xFFFE || e != 0xE000) {
                // Encapsulated pixel data may only contain fragment items;
                // elsewhere accept only if a lenient reader could not be
                // tricked into a huge allocation by this header.
                if (fragments) {
                    return corrupt("dados de pixel encapsulados corrompidos");
                }
                if (len != kUndefined && len > size_ - pos_) {
                    return corrupt("sequência corrompida");
                }
                return Walk::Stop;
            }
            if (len == kUndefined) {
                if (fragments) {
                    return corrupt("fragmento de pixel com comprimento indefinido");
                }
                const Walk w = dataset(end, explicitVr, be, depth + 1, true);
                if (w != Walk::Ok) {
                    return w;
                }
                continue;
            }
            if (len > end - pos_) {
                return corrupt("item maior que o arquivo");
            }
            const std::uint64_t itemEnd = pos_ + len;
            if (!fragments) {
                const Walk w = dataset(itemEnd, explicitVr, be, depth + 1, false);
                if (w == Walk::Corrupt) {
                    return w;
                }
            } else if (rle_ && fragmentIndex++ > 0) {
                const Walk w = rleHeader(len);
                if (w == Walk::Corrupt) {
                    return w;
                }
            }
            if (!seek(itemEnd)) {
                return Walk::Ok;
            }
        }
        return Walk::Ok;
    }

    Walk sequenceDefined(std::uint64_t end, bool explicitVr, bool be, int depth) {
        if (depth > kMaxDepth) {
            return Walk::Stop;
        }
        while (pos_ + 8 <= end) {
            std::uint16_t g = 0;
            std::uint16_t e = 0;
            std::uint32_t len = 0;
            if (!u16(g, be) || !u16(e, be) || !u32(len, be)) {
                return Walk::Ok;
            }
            if (g != 0xFFFE || e != 0xE000) {
                if (len != kUndefined && len > size_ - pos_) {
                    return corrupt("sequência corrompida");
                }
                return Walk::Stop;
            }
            if (len == kUndefined) {
                const Walk w = dataset(end, explicitVr, be, depth + 1, true);
                if (w != Walk::Ok) {
                    return w;
                }
                continue;
            }
            if (len > end - pos_) {
                return corrupt("item maior que a sequência que o contém");
            }
            const std::uint64_t itemEnd = pos_ + len;
            const Walk w = dataset(itemEnd, explicitVr, be, depth + 1, false);
            if (w == Walk::Corrupt) {
                return w;
            }
            if (!seek(itemEnd)) {
                return Walk::Ok;
            }
        }
        return Walk::Ok;
    }

private:
    std::ifstream& in_;
    std::uint64_t size_;
    std::uint64_t pos_ = 0;
    long elements_ = 0;
    bool rle_ = false;
};

}  // namespace

PreflightResult preflightDicom(const std::filesystem::path& file, std::string* reason) {
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        return PreflightResult::Corrupt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return PreflightResult::Corrupt;
    }
    Walker w(in, size);

    bool explicitVr = true;
    bool bigEndian = false;
    char magic[4] = {};
    std::uint64_t datasetStart = 0;
    if (size >= 132 && w.seek(128) && w.read(magic, 4) && std::memcmp(magic, "DICM", 4) == 0) {
        // File Meta Information: always explicit VR little endian.
        std::string ts;
        while (w.pos() + 8 <= size) {
            const std::uint64_t elemStart = w.pos();
            std::uint16_t g = 0;
            std::uint16_t e = 0;
            if (!w.u16(g, false) || !w.u16(e, false)) {
                break;
            }
            if (g != 0x0002) {
                w.seek(elemStart);
                break;
            }
            char vr[2];
            std::uint32_t len = 0;
            if (!w.read(vr, 2)) {
                break;
            }
            if (!isKnownVr(vr)) {
                if (reason != nullptr) {
                    *reason = "cabeçalho (File Meta) com VR inválida";
                }
                return PreflightResult::Corrupt;
            }
            if (isLongVr(vr)) {
                std::uint16_t reserved = 0;
                if (!w.u16(reserved, false) || !w.u32(len, false)) {
                    break;
                }
            } else {
                std::uint16_t l = 0;
                if (!w.u16(l, false)) {
                    break;
                }
                len = l;
            }
            if (len > size - w.pos()) {
                if (reason != nullptr) {
                    *reason = "cabeçalho (File Meta) corrompido";
                }
                return PreflightResult::Corrupt;
            }
            if (e == 0x0010 && len < 128) {
                std::string v(len, '\0');
                w.read(v.data(), len);
                while (!v.empty() && (v.back() == '\0' || v.back() == ' ')) {
                    v.pop_back();
                }
                ts = v;
            } else {
                w.seek(w.pos() + len);
            }
        }
        datasetStart = w.pos();
        w.setRle(ts == "1.2.840.10008.1.2.5");
        if (ts == "1.2.840.10008.1.2") {
            explicitVr = false;
        } else if (ts == "1.2.840.10008.1.2.2") {
            bigEndian = true;
        } else if (ts == "1.2.840.10008.1.2.1.99") {
            return PreflightResult::Ok;  // deflated: cannot walk without inflating
        }
    } else {
        // Raw dataset (no preamble): sniff explicit VR from the first element.
        unsigned char head[8] = {};
        w.seek(0);
        if (!w.read(head, 8)) {
            return PreflightResult::Ok;
        }
        explicitVr = std::isupper(head[4]) != 0 && std::isupper(head[5]) != 0;
        datasetStart = 0;
    }
    w.seek(datasetStart);
    const auto r = w.dataset(size, explicitVr, bigEndian, 0, false);
    if (r == Walk::Corrupt) {
        if (reason != nullptr) {
            *reason = w.reason;
        }
        return PreflightResult::Corrupt;
    }
    return PreflightResult::Ok;
}

}  // namespace vtc
