#include "dicom/DicomDecoder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>

#include <gdcmImage.h>
#include <gdcmImageApplyLookupTable.h>
#include <gdcmImageReader.h>
#include <gdcmPhotometricInterpretation.h>
#include <gdcmPixelFormat.h>
#include <gdcmSequenceOfFragments.h>

#include "core/PathUtil.h"
#include "dicom/DicomPreflight.h"

namespace vtc {

namespace {

template <class UT>
void maskStoredBits(UT* p, std::size_t n, int bitsAllocated, int bitsStored, int highBit, bool isSigned) {
    const int shift = highBit + 1 - bitsStored;
    if (shift == 0 && bitsStored == bitsAllocated) {
        return;  // nothing to do (full-width data)
    }
    const UT mask = bitsStored >= static_cast<int>(sizeof(UT) * 8) ? static_cast<UT>(~UT(0))
                                                                   : static_cast<UT>((UT(1) << bitsStored) - 1);
    const UT signBit = static_cast<UT>(UT(1) << (bitsStored - 1));
    for (std::size_t i = 0; i < n; ++i) {
        UT v = static_cast<UT>((p[i] >> shift) & mask);
        if (isSigned && (v & signBit) != 0) {
            v = static_cast<UT>(v | static_cast<UT>(~mask));
        }
        p[i] = v;
    }
}

inline std::uint8_t clamp8(double v) {
    return static_cast<std::uint8_t>(std::clamp(v + 0.5, 0.0, 255.0));
}

// YBR_FULL / YBR_FULL_422 (after GDCM up-sampling): full-range ITU-R BT.601,
// PS3.3 C.7.6.3.1.2.
void ybrFullToRgb(std::uint8_t* p, std::size_t pixels) {
    for (std::size_t i = 0; i < pixels; ++i) {
        const double y = p[3 * i];
        const double cb = p[3 * i + 1] - 128.0;
        const double cr = p[3 * i + 2] - 128.0;
        p[3 * i] = clamp8(y + 1.402 * cr);
        p[3 * i + 1] = clamp8(y - 0.344136 * cb - 0.714136 * cr);
        p[3 * i + 2] = clamp8(y + 1.772 * cb);
    }
}

// YBR_PARTIAL_422: studio range (Y 16-235, Cb/Cr 16-240).
void ybrPartialToRgb(std::uint8_t* p, std::size_t pixels) {
    for (std::size_t i = 0; i < pixels; ++i) {
        const double y = 1.164383 * (p[3 * i] - 16.0);
        const double cb = p[3 * i + 1] - 128.0;
        const double cr = p[3 * i + 2] - 128.0;
        p[3 * i] = clamp8(y + 1.596027 * cr);
        p[3 * i + 1] = clamp8(y - 0.391762 * cb - 0.812968 * cr);
        p[3 * i + 2] = clamp8(y + 2.017232 * cb);
    }
}

// Lossy JPEG processes (baseline, extended and the retired lossy ones).
bool isLossyJpegTs(const std::string& ts) {
    static const char* const kLossy[] = {"1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.52",
                                         "1.2.840.10008.1.2.4.53", "1.2.840.10008.1.2.4.54", "1.2.840.10008.1.2.4.55",
                                         "1.2.840.10008.1.2.4.56", "1.2.840.10008.1.2.4.59", "1.2.840.10008.1.2.4.60",
                                         "1.2.840.10008.1.2.4.61", "1.2.840.10008.1.2.4.62", "1.2.840.10008.1.2.4.63",
                                         "1.2.840.10008.1.2.4.64"};
    return std::any_of(std::begin(kLossy), std::end(kLossy), [&ts](const char* t) { return ts == t; });
}

struct JpegHeader {
    bool soi = false;         // stream starts with SOI
    bool sof = false;         // a start-of-frame segment was found
    int precision = 0;        // SOF sample precision
    int components = 0;       // SOF number of components
    int ids[3] = {0, 0, 0};   // first three component identifiers
    bool jfif = false;        // APP0 "JFIF"
    bool adobe = false;       // APP14 "Adobe"
    int adobeTransform = -1;  // APP14 colour transform flag
};

// Walks the JPEG marker segments up to the start of scan. Bounded and
// tolerant: it only reads inside [data, data + len).
JpegHeader scanJpegHeader(const char* data, std::size_t len) {
    JpegHeader h;
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    if (p == nullptr || len < 4 || p[0] != 0xFF || p[1] != 0xD8) {
        return h;
    }
    h.soi = true;
    std::size_t pos = 2;
    for (int guard = 0; guard < 4096 && pos + 2 <= len; ++guard) {
        if (p[pos] != 0xFF) {
            break;  // not where a marker should be
        }
        while (pos < len && p[pos] == 0xFF) {
            ++pos;  // fill bytes
        }
        if (pos >= len) {
            break;
        }
        const unsigned marker = p[pos++];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;  // stand-alone markers
        }
        if (marker == 0xD9 || marker == 0xDA) {
            break;  // EOI / start of scan: header complete
        }
        if (pos + 2 > len) {
            break;
        }
        const std::size_t segment = (static_cast<std::size_t>(p[pos]) << 8) | p[pos + 1];
        if (segment < 2 || segment > len - pos) {
            break;
        }
        const unsigned char* s = p + pos + 2;
        const std::size_t n = segment - 2;
        if (marker == 0xE0 && n >= 14 && std::memcmp(s, "JFIF", 5) == 0) {
            h.jfif = true;
        } else if (marker == 0xEE && n >= 12 && std::memcmp(s, "Adobe", 5) == 0) {
            h.adobe = true;
            h.adobeTransform = s[11];
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC &&
                   !h.sof && n >= 6) {
            h.sof = true;
            h.precision = s[0];
            h.components = s[5];
            if (h.components >= 3 && n >= 6 + 9) {
                h.ids[0] = s[6];
                h.ids[1] = s[9];
                h.ids[2] = s[12];
            }
        }
        pos += segment;
    }
    return h;
}

enum class JpegColor { Unknown, Rgb, YCbCr };

// Colour space of a 3-component JPEG stream, decided exactly as libjpeg does
// (jdapimin.c default_decompress_parms): JFIF marker -> YCbCr; Adobe APP14
// transform 0 -> RGB, otherwise YCbCr; else component ids 'R','G','B' -> RGB,
// anything else -> YCbCr. GDCM disables libjpeg's colour conversion for
// YCbCr streams, so the decoded samples are still YCbCr whatever the DICOM
// Photometric Interpretation says (files declaring RGB with a JFIF YCbCr
// stream are common, e.g. ultrasound and secondary capture).
JpegColor jpegStreamColor(const char* data, std::size_t len) {
    const JpegHeader h = scanJpegHeader(data, len);
    if (!h.sof || h.components != 3) {
        return JpegColor::Unknown;
    }
    if (h.jfif) {
        return JpegColor::YCbCr;
    }
    if (h.adobe) {
        return h.adobeTransform == 0 ? JpegColor::Rgb : JpegColor::YCbCr;
    }
    if (h.ids[0] == 'R' && h.ids[1] == 'G' && h.ids[2] == 'B') {
        return JpegColor::Rgb;
    }
    return JpegColor::YCbCr;
}

// JPEG family handled by GDCM's IJG-based codec (not JPEG-LS / JPEG 2000).
bool isIjgJpegTs(const std::string& ts) {
    static const std::string prefix = "1.2.840.10008.1.2.4.";
    if (ts.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string rest = ts.substr(prefix.size());
    if (rest.empty() || rest.size() > 2 || !std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    const int n = std::stoi(rest);
    return n >= 50 && n <= 70;
}

// Rejects JPEG streams whose frame header would drive GDCM's JPEG codec into
// an unusable state (sample precision outside 2..16 leaves it without a
// decoder and dereferences null) or that contradict the DICOM header.
bool jpegFragmentsLookSane(const gdcm::Image& image, int samplesPerPixel, std::string& why) {
    const gdcm::SequenceOfFragments* fragments = image.GetDataElement().GetSequenceOfFragments();
    if (fragments == nullptr) {
        return true;  // GDCM handles the non-encapsulated fallback itself
    }
    for (std::size_t i = 0; i < fragments->GetNumberOfFragments(); ++i) {
        const gdcm::ByteValue* bv = fragments->GetFragment(static_cast<unsigned>(i)).GetByteValue();
        if (bv == nullptr) {
            continue;
        }
        const JpegHeader h = scanJpegHeader(bv->GetPointer(), bv->GetLength());
        if (!h.soi || !h.sof) {
            continue;  // continuation fragment or header split across fragments
        }
        if (h.precision < 2 || h.precision > 16) {
            why = "precisão JPEG inválida (" + std::to_string(h.precision) + " bits)";
            return false;
        }
        if (h.components != samplesPerPixel) {
            why = "número de componentes do JPEG diferente do cabeçalho DICOM";
            return false;
        }
    }
    return true;
}

enum class ColorConversion { None, YbrFull, YbrPartial };

ColorConversion colorConversionFor(const gdcm::Image& image, const std::string& ts) {
    const auto pi = image.GetPhotometricInterpretation();
    ColorConversion byPi = ColorConversion::None;
    if (pi == gdcm::PhotometricInterpretation::YBR_FULL || pi == gdcm::PhotometricInterpretation::YBR_FULL_422) {
        byPi = ColorConversion::YbrFull;
    } else if (pi == gdcm::PhotometricInterpretation::YBR_PARTIAL_422) {
        byPi = ColorConversion::YbrPartial;
    }
    // YBR_ICT / YBR_RCT: OpenJPEG already applied the inverse component
    // transform, the decoded samples are RGB.
    if (!isLossyJpegTs(ts)) {
        return byPi;
    }
    const gdcm::SequenceOfFragments* fragments = image.GetDataElement().GetSequenceOfFragments();
    if (fragments == nullptr || fragments->GetNumberOfFragments() == 0) {
        return byPi;
    }
    const gdcm::ByteValue* bv = fragments->GetFragment(0).GetByteValue();
    if (bv == nullptr) {
        return byPi;
    }
    switch (jpegStreamColor(bv->GetPointer(), bv->GetLength())) {
        case JpegColor::YCbCr: return ColorConversion::YbrFull;  // JPEG YCbCr is always full range
        case JpegColor::Rgb: return ColorConversion::None;
        case JpegColor::Unknown: break;
    }
    return byPi;
}

}  // namespace

DecodeResult decodeInstance(const InstanceInfo& info, const ParseLimits& limits) {
    DecodeResult result;
    if (info.pixelDataTruncated) {
        result.error = "Arquivo truncado: os dados de pixel estão incompletos.";
        return result;
    }
    try {
        // The file may have changed since it was scanned: re-check structure.
        if (std::string why; preflightDicom(utf8ToPath(info.filePath), &why) == PreflightResult::Corrupt) {
            result.error = "Arquivo DICOM corrompido (" + why + ").";
            return result;
        }
        std::ifstream stream(utf8ToPath(info.filePath), std::ios::binary);
        if (!stream) {
            result.error = "Arquivo não encontrado ou inacessível.";
            return result;
        }
        gdcm::ImageReader reader;
        reader.SetStream(stream);
        if (!reader.Read()) {
            result.error = "Não foi possível ler os dados de pixel (Transfer Syntax: " +
                           transferSyntaxName(info.transferSyntaxUid) + ").";
            return result;
        }
        const gdcm::Image& original = reader.GetImage();

        const unsigned columns = original.GetColumns();
        const unsigned rows = original.GetRows();
        const unsigned frames = original.GetNumberOfDimensions() >= 3 ? original.GetDimension(2) : 1u;
        if (static_cast<int>(columns) != info.columns || static_cast<int>(rows) != info.rows || columns == 0 ||
            rows == 0 || static_cast<int>(columns) > limits.maxColumns || static_cast<int>(rows) > limits.maxRows ||
            frames == 0 || static_cast<int>(frames) > limits.maxFrames) {
            result.error = "Dimensões da imagem inconsistentes com o cabeçalho DICOM.";
            return result;
        }

        if (isIjgJpegTs(info.transferSyntaxUid)) {
            std::string why;
            if (!jpegFragmentsLookSane(original, original.GetPixelFormat().GetSamplesPerPixel(), why)) {
                result.error = "Fluxo JPEG corrompido (" + why + ").";
                return result;
            }
        }

        // Palette color -> RGB through GDCM's LUT filter.
        std::unique_ptr<gdcm::ImageApplyLookupTable> lutFilter;
        const gdcm::Image* image = &original;
        if (original.GetPhotometricInterpretation() == gdcm::PhotometricInterpretation::PALETTE_COLOR) {
            lutFilter = std::make_unique<gdcm::ImageApplyLookupTable>();
            lutFilter->SetInput(original);
            if (!lutFilter->Apply()) {
                result.error = "Não foi possível aplicar a tabela de cores (PALETTE COLOR).";
                return result;
            }
            image = &lutFilter->GetOutput();
        }

        const gdcm::PixelFormat& pf = image->GetPixelFormat();
        const int spp = pf.GetSamplesPerPixel();
        const int bitsAllocated = pf.GetBitsAllocated();
        const int bitsStored = pf.GetBitsStored();
        const int highBit = pf.GetHighBit();
        const bool isSigned = pf.GetPixelRepresentation() == 1;
        if ((spp != 1 && spp != 3) ||
            (bitsAllocated != 1 && bitsAllocated != 8 && bitsAllocated != 16 && bitsAllocated != 32) ||
            bitsStored < 1 || bitsStored > bitsAllocated || highBit < bitsStored - 1 || highBit >= bitsAllocated) {
            result.error = "Formato de pixel não suportado após decodificação.";
            return result;
        }
        if (pf.GetScalarType() == gdcm::PixelFormat::FLOAT32 || pf.GetScalarType() == gdcm::PixelFormat::FLOAT64) {
            result.error = "Dados de pixel em ponto flutuante não são suportados nesta versão.";
            return result;
        }

        const std::uint64_t pixelsPerFrame = static_cast<std::uint64_t>(columns) * rows;
        const std::uint64_t bytesPerSample = bitsAllocated == 1 ? 1 : static_cast<std::uint64_t>(bitsAllocated / 8);
        const std::uint64_t expected = bitsAllocated == 1 ? (pixelsPerFrame * frames + 7) / 8
                                                          : pixelsPerFrame * frames * static_cast<std::uint64_t>(spp) *
                                                                bytesPerSample;
        if (expected > limits.maxPixelBytes) {
            result.error = "Imagem descomprimida excederia o limite de memória configurado.";
            return result;
        }
        const unsigned long length = image->GetBufferLength();
        if (length < expected) {
            result.error = "Dados de pixel incompletos ou corrompidos.";
            return result;
        }
        std::vector<char> buffer(length);
        if (!image->GetBuffer(buffer.data())) {
            result.error = "Falha ao decodificar os pixels (Transfer Syntax: " +
                           transferSyntaxName(info.transferSyntaxUid) + ").";
            return result;
        }

        const auto pi = image->GetPhotometricInterpretation();
        const bool planar = image->GetPlanarConfiguration() == 1;
        // GDCM returns YBR samples untouched (native, RLE, lossless JPEG and
        // JPEG-LS by the declared PI; lossy JPEG by the stream's own colour
        // space), so the conversion to RGB happens here.
        const ColorConversion colorConversion =
            spp == 3 ? colorConversionFor(*image, info.transferSyntaxUid) : ColorConversion::None;
        const bool mono1 = pi == gdcm::PhotometricInterpretation::MONOCHROME1;

        result.frames.reserve(frames);
        for (unsigned f = 0; f < frames; ++f) {
            auto frame = std::make_shared<DecodedFrame>();
            frame->width = static_cast<int>(columns);
            frame->height = static_cast<int>(rows);
            frame->monochrome1 = mono1;
            frame->bitsStored = bitsStored;
            const std::size_t npix = static_cast<std::size_t>(pixelsPerFrame);
            const auto& finfo = info.frames.at(std::min<std::size_t>(f, info.frames.size() - 1));
            frame->slope = finfo.rescaleSlope;
            frame->intercept = finfo.rescaleIntercept;

            if (spp == 3) {
                frame->format = PixelFormat::RGB8;
                frame->data.resize(npix * 3);
                const std::size_t frameBytes = npix * 3 * bytesPerSample;
                const char* src = buffer.data() + frameBytes * f;
                for (std::size_t i = 0; i < npix; ++i) {
                    for (int c = 0; c < 3; ++c) {
                        const std::size_t idx = planar ? static_cast<std::size_t>(c) * npix + i : i * 3 + c;
                        std::uint8_t v = 0;
                        if (bytesPerSample == 1) {
                            v = static_cast<std::uint8_t>(src[idx]);
                        } else {
                            std::uint16_t w = 0;
                            std::memcpy(&w, src + idx * 2, 2);
                            v = static_cast<std::uint8_t>(w >> std::max(0, bitsStored - 8));
                        }
                        frame->data[i * 3 + static_cast<std::size_t>(c)] = v;
                    }
                }
                if (colorConversion == ColorConversion::YbrFull) {
                    ybrFullToRgb(frame->data.data(), npix);
                } else if (colorConversion == ColorConversion::YbrPartial) {
                    ybrPartialToRgb(frame->data.data(), npix);
                }
                frame->slope = 1.0;
                frame->intercept = 0.0;
            } else if (bitsAllocated == 1) {
                frame->format = PixelFormat::U8;
                frame->data.resize(npix);
                const std::uint64_t bitBase = pixelsPerFrame * f;
                for (std::size_t i = 0; i < npix; ++i) {
                    const std::uint64_t bit = bitBase + i;
                    frame->data[i] = (static_cast<unsigned char>(buffer[bit / 8]) >> (bit % 8)) & 1u;
                }
                frame->bitsStored = 1;
            } else if (bitsAllocated == 8) {
                frame->format = isSigned ? PixelFormat::I8 : PixelFormat::U8;
                frame->data.resize(npix);
                std::memcpy(frame->data.data(), buffer.data() + npix * f, npix);
                maskStoredBits(frame->data.data(), npix, 8, bitsStored, highBit, isSigned);
            } else if (bitsAllocated == 16) {
                frame->format = isSigned ? PixelFormat::I16 : PixelFormat::U16;
                frame->data.resize(npix * 2);
                std::memcpy(frame->data.data(), buffer.data() + npix * 2 * f, npix * 2);
                maskStoredBits(frame->as<std::uint16_t>(), npix, 16, bitsStored, highBit, isSigned);
            } else {  // 32
                frame->format = isSigned ? PixelFormat::I32 : PixelFormat::U32;
                frame->data.resize(npix * 4);
                std::memcpy(frame->data.data(), buffer.data() + npix * 4 * f, npix * 4);
                maskStoredBits(frame->as<std::uint32_t>(), npix, 32, bitsStored, highBit, isSigned);
            }
            frame->computeRange();
            result.frames.push_back(std::move(frame));
        }
    } catch (const std::bad_alloc&) {
        result.frames.clear();
        result.error = "Memória insuficiente para decodificar a imagem.";
    } catch (...) {
        result.frames.clear();
        result.error = "Erro inesperado ao decodificar a imagem.";
    }
    return result;
}

}  // namespace vtc
