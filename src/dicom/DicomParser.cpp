#include "dicom/DicomParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

#include <gdcmConfigure.h>
#include <gdcmDataSet.h>
#include <gdcmFileMetaInformation.h>
#include <gdcmGlobal.h>
#include <gdcmTrace.h>
#include <gdcmItem.h>
#include <gdcmReader.h>
#include <gdcmSequenceOfItems.h>
#include <gdcmTag.h>
#include <gdcmTransferSyntax.h>

#include "core/PathUtil.h"
#include "dicom/DicomPreflight.h"
#include "dicom/TextUtil.h"

namespace vtc {

namespace {

constexpr const char* kDicomDirSopClass = "1.2.840.10008.1.3.10";

struct TsEntry {
    const char* uid;
    const char* name;
    bool supported;
    bool lossy;
};

constexpr std::array<TsEntry, 25> kTransferSyntaxes{{
    {"1.2.840.10008.1.2", "Implicit VR Little Endian", true, false},
    {"1.2.840.10008.1.2.1", "Explicit VR Little Endian", true, false},
    {"1.2.840.10008.1.2.1.99", "Deflated Explicit VR Little Endian", true, false},
    {"1.2.840.10008.1.2.2", "Explicit VR Big Endian (retired)", true, false},
    {"1.2.840.10008.1.2.4.50", "JPEG Baseline (Process 1)", true, true},
    {"1.2.840.10008.1.2.4.51", "JPEG Extended (Process 2 & 4)", true, true},
    {"1.2.840.10008.1.2.4.57", "JPEG Lossless (Process 14)", true, false},
    {"1.2.840.10008.1.2.4.70", "JPEG Lossless SV1 (Process 14, SV1)", true, false},
    {"1.2.840.10008.1.2.4.80", "JPEG-LS Lossless", true, false},
    {"1.2.840.10008.1.2.4.81", "JPEG-LS Near-Lossless", true, true},
    {"1.2.840.10008.1.2.4.90", "JPEG 2000 (Lossless Only)", true, false},
    {"1.2.840.10008.1.2.4.91", "JPEG 2000", true, true},
    {"1.2.840.10008.1.2.4.92", "JPEG 2000 Part 2 Multi-component (Lossless Only)", false, false},
    {"1.2.840.10008.1.2.4.93", "JPEG 2000 Part 2 Multi-component", false, true},
    {"1.2.840.10008.1.2.5", "RLE Lossless", true, false},
    {"1.2.840.10008.1.2.4.100", "MPEG2 Main Profile @ Main Level", false, true},
    {"1.2.840.10008.1.2.4.101", "MPEG2 Main Profile @ High Level", false, true},
    {"1.2.840.10008.1.2.4.102", "MPEG-4 AVC/H.264 High Profile", false, true},
    {"1.2.840.10008.1.2.4.103", "MPEG-4 AVC/H.264 BD-compatible", false, true},
    {"1.2.840.10008.1.2.4.107", "HEVC/H.265 Main Profile", false, true},
    {"1.2.840.10008.1.2.4.108", "HEVC/H.265 Main 10 Profile", false, true},
    {"1.2.840.10008.1.2.4.201", "High-Throughput JPEG 2000 (Lossless Only)", false, false},
    {"1.2.840.10008.1.2.4.202", "High-Throughput JPEG 2000 with RPCL (Lossless Only)", false, false},
    {"1.2.840.10008.1.2.4.203", "High-Throughput JPEG 2000", false, true},
    {"1.2.840.10008.1.2.4.110", "JPEG XL Lossless", false, false},
}};

const TsEntry* findTs(const std::string& uid) {
    for (const auto& e : kTransferSyntaxes) {
        if (uid == e.uid) {
            return &e;
        }
    }
    return nullptr;
}

// ---- raw attribute access -------------------------------------------------

struct SeqView {
    gdcm::SmartPointer<gdcm::SequenceOfItems> sq;
    std::vector<const gdcm::DataSet*> items;
    [[nodiscard]] auto begin() const { return items.begin(); }
    [[nodiscard]] auto end() const { return items.end(); }
    [[nodiscard]] bool empty() const { return items.empty(); }
    [[nodiscard]] size_t size() const { return items.size(); }
    [[nodiscard]] const gdcm::DataSet* operator[](size_t i) const { return items[i]; }
    [[nodiscard]] const gdcm::DataSet* front() const { return items.front(); }
};

class Access {
public:
    Access(const gdcm::DataSet& ds, bool bigEndian, const std::string& charset)
        : ds_(ds), bigEndian_(bigEndian), charset_(charset) {}

    [[nodiscard]] const gdcm::DataElement* element(std::uint16_t g, std::uint16_t e) const {
        const gdcm::Tag t(g, e);
        if (!ds_.FindDataElement(t)) {
            return nullptr;
        }
        const gdcm::DataElement& de = ds_.GetDataElement(t);
        return &de;
    }

    [[nodiscard]] std::string raw(std::uint16_t g, std::uint16_t e) const {
        const auto* de = element(g, e);
        if (de == nullptr) {
            return {};
        }
        const gdcm::ByteValue* bv = de->GetByteValue();
        if (bv == nullptr || bv->GetPointer() == nullptr) {
            return {};
        }
        return std::string(bv->GetPointer(), bv->GetLength());
    }

    [[nodiscard]] std::string str(std::uint16_t g, std::uint16_t e) const { return trimDicom(raw(g, e)); }
    [[nodiscard]] std::string text(std::uint16_t g, std::uint16_t e) const {
        return dicomToUtf8(trimDicom(raw(g, e)), charset_);
    }
    [[nodiscard]] std::optional<double> ds(std::uint16_t g, std::uint16_t e) const {
        const auto values = parseDoubles(raw(g, e));
        if (values.empty()) {
            return std::nullopt;
        }
        return values.front();
    }
    [[nodiscard]] std::vector<double> dsMulti(std::uint16_t g, std::uint16_t e) const { return parseDoubles(raw(g, e)); }
    [[nodiscard]] std::optional<int> is(std::uint16_t g, std::uint16_t e) const {
        const auto parts = splitBackslash(raw(g, e));
        return parts.empty() ? std::nullopt : parseInt(parts.front());
    }
    // Binary unsigned short (US) value, first of VM.
    [[nodiscard]] std::optional<int> us(std::uint16_t g, std::uint16_t e, int index = 0) const {
        const std::string r = raw(g, e);
        const size_t off = static_cast<size_t>(index) * 2;
        if (r.size() < off + 2) {
            return std::nullopt;
        }
        const auto b0 = static_cast<unsigned char>(r[off]);
        const auto b1 = static_cast<unsigned char>(r[off + 1]);
        return bigEndian_ ? (b0 << 8) | b1 : (b1 << 8) | b0;
    }
    // Binary double (FD) value.
    [[nodiscard]] std::optional<double> fd(std::uint16_t g, std::uint16_t e) const {
        const std::string r = raw(g, e);
        if (r.size() < 8 || bigEndian_) {
            return std::nullopt;
        }
        double v = 0.0;
        std::memcpy(&v, r.data(), sizeof(v));
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
        return v;
    }
    // Binary unsigned long (UL).
    [[nodiscard]] std::optional<std::uint32_t> ul(std::uint16_t g, std::uint16_t e) const {
        const std::string r = raw(g, e);
        if (r.size() < 4 || bigEndian_) {
            return std::nullopt;
        }
        std::uint32_t v = 0;
        std::memcpy(&v, r.data(), sizeof(v));
        return v;
    }

    // Returns the nested datasets of a sequence (empty when absent/not SQ).
    // The returned view owns a reference to the parsed sequence: for implicit
    // VR files GDCM parses the sequence on demand into a new object, so the
    // item pointers are only valid while the view is alive.
    [[nodiscard]] SeqView items(std::uint16_t g, std::uint16_t e) const {
        SeqView view;
        const auto* de = element(g, e);
        if (de == nullptr || de->IsEmpty()) {
            return view;
        }
        try {
            view.sq = de->GetValueAsSQ();
        } catch (...) {
            view.sq = nullptr;
        }
        if (!view.sq) {
            return view;
        }
        const auto n = view.sq->GetNumberOfItems();
        view.items.reserve(n);
        for (gdcm::SequenceOfItems::SizeType i = 1; i <= n; ++i) {
            view.items.push_back(&view.sq->GetItem(i).GetNestedDataSet());
        }
        return view;
    }

    [[nodiscard]] Access nested(const gdcm::DataSet& d) const { return {d, bigEndian_, charset_}; }

private:
    const gdcm::DataSet& ds_;
    bool bigEndian_;
    const std::string& charset_;
};

void readPlaneAttributes(const Access& a, FrameGeometry& g) {
    const auto ipp = a.dsMulti(0x0020, 0x0032);
    if (ipp.size() == 3) {
        g.hasPosition = true;
        g.position = {ipp[0], ipp[1], ipp[2]};
    }
    const auto iop = a.dsMulti(0x0020, 0x0037);
    if (iop.size() == 6) {
        const Vec3 r{iop[0], iop[1], iop[2]};
        const Vec3 c{iop[3], iop[4], iop[5]};
        // Direction cosines are often rounded to a few decimals. Accept small
        // deviations (< ~0.6 degree) and make the basis exactly orthonormal
        // (Gram-Schmidt, keeping the row direction), because every geometric
        // computation (patient<->pixel, MPR planes, reference lines) assumes
        // it. Degenerate or clearly skewed vectors are not trusted.
        if (r.norm() > 0.5 && c.norm() > 0.5) {
            const Vec3 rn = r.normalized();
            const Vec3 cn = c.normalized();
            const double dot = rn.dot(cn);
            if (std::abs(dot) < 0.01) {
                g.hasOrientation = true;
                g.rowDir = rn;
                g.colDir = (cn - rn * dot).normalized();
            }
        }
    }
}

bool readPixelSpacing(const Access& a, FrameGeometry& g, SpacingSource source) {
    const auto ps = a.dsMulti(0x0028, 0x0030);
    if (ps.size() == 2 && ps[0] > 0.0 && ps[1] > 0.0) {
        g.spacingY = ps[0];  // between rows
        g.spacingX = ps[1];  // between columns
        g.spacingSource = source;
        return true;
    }
    return false;
}

void readWindows(const Access& a, std::vector<WindowSetting>& out) {
    const auto centers = a.dsMulti(0x0028, 0x1050);
    const auto widths = a.dsMulti(0x0028, 0x1051);
    const auto expl = splitBackslash(a.text(0x0028, 0x1055));
    const size_t n = std::min(centers.size(), widths.size());
    if (n == 0) {
        return;
    }
    out.clear();
    for (size_t i = 0; i < n; ++i) {
        if (widths[i] < 1.0) {
            continue;  // invalid per PS3.3 C.11.2.1.2: width must be >= 1
        }
        WindowSetting w;
        w.center = centers[i];
        w.width = widths[i];
        if (i < expl.size()) {
            w.explanation = expl[i];
        }
        out.push_back(w);
    }
}

void readRescale(const Access& a, FrameInfo& f) {
    if (auto s = a.ds(0x0028, 0x1053)) {
        if (*s != 0.0) {
            f.rescaleSlope = *s;
        }
    }
    if (auto i = a.ds(0x0028, 0x1052)) {
        f.rescaleIntercept = *i;
    }
    const std::string type = a.str(0x0028, 0x1054);
    if (!type.empty()) {
        f.rescaleType = type;
    }
}

// Applies one Functional Group (shared or per-frame) to a frame.
void applyFunctionalGroup(const Access& fg, FrameInfo& f) {
    for (const auto* d : fg.items(0x0028, 0x9110)) {  // Pixel Measures
        const Access a = fg.nested(*d);
        readPixelSpacing(a, f.geometry, SpacingSource::EnhancedPixelMeasures);
        if (auto t = a.ds(0x0018, 0x0050)) {
            f.geometry.sliceThickness = t;
        }
    }
    for (const auto* d : fg.items(0x0020, 0x9113)) {  // Plane Position (Patient)
        const Access a = fg.nested(*d);
        const auto ipp = a.dsMulti(0x0020, 0x0032);
        if (ipp.size() == 3) {
            f.geometry.hasPosition = true;
            f.geometry.position = {ipp[0], ipp[1], ipp[2]};
        }
    }
    for (const auto* d : fg.items(0x0020, 0x9116)) {  // Plane Orientation (Patient)
        FrameGeometry tmp = f.geometry;
        readPlaneAttributes(fg.nested(*d), tmp);
        if (tmp.hasOrientation) {
            f.geometry.hasOrientation = true;
            f.geometry.rowDir = tmp.rowDir;
            f.geometry.colDir = tmp.colDir;
        }
    }
    for (const auto* d : fg.items(0x0028, 0x9145)) {  // Pixel Value Transformation
        readRescale(fg.nested(*d), f);
    }
    for (const auto* d : fg.items(0x0028, 0x9132)) {  // Frame VOI LUT
        readWindows(fg.nested(*d), f.windows);
    }
    for (const auto* d : fg.items(0x0020, 0x9111)) {  // Frame Content
        const Access a = fg.nested(*d);
        if (auto tp = a.ul(0x0020, 0x9128)) {
            f.keys.temporalPosition = static_cast<int>(*tp);
        }
        const std::string stack = a.str(0x0020, 0x9056);
        if (!stack.empty()) {
            f.keys.stackId = stack;
        }
    }
    for (const auto* d : fg.items(0x0018, 0x9114)) {  // MR Echo
        if (auto te = fg.nested(*d).fd(0x0018, 0x9082)) {
            f.keys.echoTime = te;
        }
    }
    for (const auto* d : fg.items(0x0018, 0x9117)) {  // MR Diffusion
        if (auto b = fg.nested(*d).fd(0x0018, 0x9087)) {
            f.keys.diffusionBValue = b;
        }
    }
    for (const auto* d : fg.items(0x0018, 0x9118)) {  // Cardiac Synchronization
        if (auto tt = fg.nested(*d).fd(0x0020, 0x9153)) {
            f.keys.triggerTime = tt;
        }
    }
}

void readUltrasoundCalibration(const Access& a, FrameGeometry& g) {
    for (const auto* d : a.items(0x0018, 0x6011)) {  // Sequence of Ultrasound Regions
        const Access r = a.nested(*d);
        // Only 2D tissue/flow regions calibrate distances in the image plane
        // (M-mode, spectral Doppler and waveform regions use time axes).
        const auto spatialFormat = r.us(0x0018, 0x6012);
        if (spatialFormat && *spatialFormat != 1) {
            continue;
        }
        const auto unitsX = r.us(0x0018, 0x6024);
        const auto unitsY = r.us(0x0018, 0x6026);
        const auto dx = r.fd(0x0018, 0x602C);
        const auto dy = r.fd(0x0018, 0x602E);
        // Units code 3 = cm. Use the first spatially calibrated region.
        if (unitsX == 3 && unitsY == 3 && dx && dy && *dx > 0.0 && *dy > 0.0) {
            g.spacingX = std::abs(*dx) * 10.0;
            g.spacingY = std::abs(*dy) * 10.0;
            g.spacingSource = SpacingSource::UltrasoundRegion;
            return;
        }
    }
}

std::optional<VoiLutTable> readVoiLut(const Access& a, bool signedPixels) {
    const SeqView seq = a.items(0x0028, 0x3010);
    if (seq.empty()) {
        return std::nullopt;
    }
    const Access item = a.nested(*seq.front());
    const auto entriesRaw = item.us(0x0028, 0x3002, 0);
    const auto firstRaw = item.us(0x0028, 0x3002, 1);
    const auto bits = item.us(0x0028, 0x3002, 2);
    if (!entriesRaw || !firstRaw || !bits || *bits < 8 || *bits > 16) {
        return std::nullopt;
    }
    const int entries = *entriesRaw == 0 ? 65536 : *entriesRaw;
    VoiLutTable lut;
    lut.firstMapped = signedPixels ? static_cast<std::int16_t>(*firstRaw) : *firstRaw;
    lut.bitsPerEntry = *bits;
    lut.explanation = item.text(0x0028, 0x3003);
    const std::string data = item.raw(0x0028, 0x3006);
    if (data.size() == static_cast<size_t>(entries) * 2) {
        lut.data.resize(static_cast<size_t>(entries));
        std::memcpy(lut.data.data(), data.data(), data.size());
    } else if (*bits == 8 && data.size() == static_cast<size_t>(entries)) {
        lut.data.resize(static_cast<size_t>(entries));
        for (int i = 0; i < entries; ++i) {
            lut.data[static_cast<size_t>(i)] = static_cast<unsigned char>(data[static_cast<size_t>(i)]);
        }
    } else {
        return std::nullopt;
    }
    return lut;
}

}  // namespace

std::string transferSyntaxName(const std::string& uid) {
    if (const auto* e = findTs(uid)) {
        return e->name;
    }
    return uid.empty() ? std::string("(desconhecido)") : uid;
}

std::string gdcmVersion() { return GDCM_VERSION; }

void initializeDicomLibrary() {
    gdcm::Trace::WarningOff();
    gdcm::Trace::ErrorOff();
    gdcm::Trace::DebugOff();
    (void)gdcm::Global::GetInstance();
}

bool isTransferSyntaxSupported(const std::string& uid) {
    if (uid.empty()) {
        return true;  // no meta header: GDCM assumes implicit little endian
    }
    const auto* e = findTs(uid);
    return e != nullptr && e->supported;
}

bool looksLikeDicom(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::array<unsigned char, 132> buf{};
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return looksLikeDicomBytes(buf.data(), static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
}

bool looksLikeDicomBytes(const unsigned char* buf, std::size_t got) {
    if (buf == nullptr) {
        return false;
    }
    if (got >= 132 && buf[128] == 'D' && buf[129] == 'I' && buf[130] == 'C' && buf[131] == 'M') {
        return true;
    }
    if (got < 8) {
        return false;
    }
    // Raw dataset without preamble: first element should be (0002,xxxx) or
    // (0008,xxxx) in little endian, with either explicit VR letters or a sane
    // implicit length.
    const int group = buf[0] | (buf[1] << 8);
    if (group != 0x0002 && group != 0x0008) {
        return false;
    }
    const bool explicitVr = std::isupper(buf[4]) != 0 && std::isupper(buf[5]) != 0;
    if (explicitVr) {
        return true;
    }
    const std::uint32_t len = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (static_cast<std::uint32_t>(buf[7]) << 24);
    return len < 4096;
}

ParseResult parseDicomHeader(const std::filesystem::path& file, const ParseLimits& limits) {
    ParseResult result;
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        result.status = ParseStatus::NotDicom;
        result.message = "Arquivo inacessível.";
        return result;
    }
    if (size > limits.maxFileBytes) {
        result.status = ParseStatus::Unsupported;
        result.message = "Arquivo excede o tamanho máximo configurado.";
        return result;
    }
    if (!looksLikeDicom(file)) {
        result.status = ParseStatus::NotDicom;
        result.message = "Este arquivo não pôde ser interpretado como DICOM.";
        return result;
    }

    if (std::string why; preflightDicom(file, &why) == PreflightResult::Corrupt) {
        result.status = ParseStatus::Malformed;
        result.message = "Arquivo DICOM corrompido (" + why + ").";
        return result;
    }

    // Open through std::ifstream(path) so Unicode paths work on Windows.
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        result.status = ParseStatus::NotDicom;
        result.message = "Arquivo inacessível.";
        return result;
    }
    gdcm::Reader reader;
    reader.SetStream(stream);
    bool ok = false;
    try {
        std::set<gdcm::Tag> skip;
        skip.insert(gdcm::Tag(0x7fe0, 0x0010));
        ok = reader.ReadUpToTag(gdcm::Tag(0x7fe0, 0x0010), skip);
    } catch (...) {
        ok = false;
    }
    if (!ok) {
        result.status = ParseStatus::Malformed;
        result.message = "Este arquivo não pôde ser interpretado como DICOM.";
        return result;
    }

    const gdcm::File& gfile = reader.GetFile();
    const gdcm::DataSet& dataset = gfile.GetDataSet();
    const gdcm::FileMetaInformation& meta = gfile.GetHeader();

    auto info = std::make_shared<InstanceInfo>();
    info->filePath = pathToUtf8(file);
    info->fileSize = size;

    const gdcm::TransferSyntax ts = meta.GetDataSetTransferSyntax();
    const char* tsUid = ts.GetString();
    info->transferSyntaxUid = tsUid != nullptr ? tsUid : "";
    // GDCM byte-swaps binary values of Big Endian files to native order while
    // reading, so values are always interpreted as little endian here.
    const bool bigEndian = false;

    // Character set must be read first: it governs all text attributes.
    {
        const std::string emptyCharset;
        const Access pre(dataset, bigEndian, emptyCharset);
        info->specificCharacterSet = pre.str(0x0008, 0x0005);
    }
    const Access a(dataset, bigEndian, info->specificCharacterSet);

    info->sopClassUid = a.str(0x0008, 0x0016);
    if (info->sopClassUid.empty()) {
        const Access m(meta, false, info->specificCharacterSet);
        info->sopClassUid = m.str(0x0002, 0x0002);
    }
    info->sopInstanceUid = a.str(0x0008, 0x0018);
    if (info->sopClassUid == kDicomDirSopClass) {
        result.status = ParseStatus::NonImage;
        result.message = "DICOMDIR (índice de mídia) ignorado.";
        return result;
    }

    info->patientName = formatPersonName(a.text(0x0010, 0x0010));
    info->patientId = a.text(0x0010, 0x0020);
    info->patientBirthDate = a.str(0x0010, 0x0030);
    info->patientSex = a.str(0x0010, 0x0040);
    info->patientAge = a.str(0x0010, 0x1010);

    info->studyInstanceUid = a.str(0x0020, 0x000D);
    info->studyDate = a.str(0x0008, 0x0020);
    info->studyTime = a.str(0x0008, 0x0030);
    info->studyDescription = a.text(0x0008, 0x1030);
    info->accessionNumber = a.text(0x0008, 0x0050);
    info->institutionName = a.text(0x0008, 0x0080);
    info->referringPhysician = formatPersonName(a.text(0x0008, 0x0090));

    info->seriesInstanceUid = a.str(0x0020, 0x000E);
    info->seriesDescription = a.text(0x0008, 0x103E);
    info->modality = a.str(0x0008, 0x0060);
    info->bodyPartExamined = a.text(0x0018, 0x0015);
    info->manufacturer = a.text(0x0008, 0x0070);
    info->manufacturerModel = a.text(0x0008, 0x1090);
    info->protocolName = a.text(0x0018, 0x1030);
    info->patientPosition = a.str(0x0018, 0x5100);
    info->frameOfReferenceUid = a.str(0x0020, 0x0052);
    info->seriesNumber = a.is(0x0020, 0x0011);
    info->seriesDate = a.str(0x0008, 0x0021);
    info->seriesTime = a.str(0x0008, 0x0031);

    info->instanceNumber = a.is(0x0020, 0x0013);
    info->acquisitionTime = a.str(0x0008, 0x0032);
    info->contentTime = a.str(0x0008, 0x0033);
    info->spacingBetweenSlices = a.ds(0x0018, 0x0088);
    info->kvp = a.ds(0x0018, 0x0060);
    if (auto fps = a.is(0x0008, 0x2144); fps && *fps > 0) {  // Recommended Display Frame Rate
        info->recommendedFrameRate = *fps;
    } else if (auto cine = a.ds(0x0018, 0x0040); cine && *cine > 0) {  // Cine Rate
        info->recommendedFrameRate = cine;
    }
    info->frameTimeMs = a.ds(0x0018, 0x1063);

    const std::string imageType = a.str(0x0008, 0x0008);
    info->hasLocalizerImageType = imageType.find("LOCALIZER") != std::string::npos;
    info->lossyCompressed = a.str(0x0028, 0x2110) == "01";
    if (const auto* e = findTs(info->transferSyntaxUid); e != nullptr && e->lossy) {
        info->lossyCompressed = true;
    }

    // ---- Image Pixel module ----
    info->rows = a.us(0x0028, 0x0010).value_or(0);
    info->columns = a.us(0x0028, 0x0011).value_or(0);
    info->samplesPerPixel = a.us(0x0028, 0x0002).value_or(1);
    info->bitsAllocated = a.us(0x0028, 0x0100).value_or(0);
    info->bitsStored = a.us(0x0028, 0x0101).value_or(info->bitsAllocated);
    info->highBit = a.us(0x0028, 0x0102).value_or(info->bitsStored - 1);
    info->pixelRepresentation = a.us(0x0028, 0x0103).value_or(0);
    info->planarConfiguration = a.us(0x0028, 0x0006).value_or(0);
    info->photometricInterpretation = a.str(0x0028, 0x0004);
    info->voiLutFunction = a.str(0x0028, 0x1056);
    const int frames = a.is(0x0028, 0x0008).value_or(1);
    info->numberOfFrames = frames;

    // ReadUpToTag stops right after the Pixel Data element header, so the
    // stream position is the offset of the pixel value (when present).
    const std::uint64_t stopPos = static_cast<std::uint64_t>(reader.GetStreamCurrentPosition());
    const bool reachedPixelData = stopPos > 0 && stopPos < size;
    const bool pixelElementPresent = reachedPixelData || dataset.FindDataElement(gdcm::Tag(0x7fe0, 0x0010)) ||
                                     dataset.FindDataElement(gdcm::Tag(0x7fe0, 0x0008)) ||
                                     dataset.FindDataElement(gdcm::Tag(0x7fe0, 0x0009));
    const bool pixelModulePresent = info->rows > 0 && info->columns > 0 && info->bitsAllocated > 0;
    info->hasPixelData = pixelModulePresent && (pixelElementPresent || !info->photometricInterpretation.empty());

    const bool floatPixels = !reachedPixelData && (dataset.FindDataElement(gdcm::Tag(0x7fe0, 0x0008)) ||
                                                   dataset.FindDataElement(gdcm::Tag(0x7fe0, 0x0009)));
    if (info->hasPixelData && floatPixels) {
        result.status = ParseStatus::Unsupported;
        result.message = "Dados de pixel em ponto flutuante (Float/Double Float Pixel Data, ex.: mapas paramétricos) "
                         "não são suportados nesta versão.";
        return result;
    }

    if (!info->hasPixelData) {
        result.status = ParseStatus::NonImage;
        result.message = "Objeto DICOM sem imagem (" + info->modality + ").";
        result.instance = info;
        return result;
    }

    // ---- sanity limits (before anything is ever allocated) ----
    if (info->rows > limits.maxRows || info->columns > limits.maxColumns || frames < 1 ||
        frames > limits.maxFrames) {
        result.status = ParseStatus::Unsupported;
        result.message = "Dimensões da imagem fora dos limites suportados.";
        return result;
    }
    const bool bitsOk = (info->bitsAllocated == 1 || info->bitsAllocated == 8 || info->bitsAllocated == 16 ||
                         info->bitsAllocated == 32 || info->bitsAllocated == 64) &&
                        info->bitsStored >= 1 && info->bitsStored <= info->bitsAllocated &&
                        info->highBit >= info->bitsStored - 1 && info->highBit < info->bitsAllocated;
    if (!bitsOk || (info->samplesPerPixel != 1 && info->samplesPerPixel != 3)) {
        result.status = ParseStatus::Unsupported;
        result.message = "Formato de pixel não suportado (BitsAllocated=" + std::to_string(info->bitsAllocated) +
                         ", BitsStored=" + std::to_string(info->bitsStored) +
                         ", SamplesPerPixel=" + std::to_string(info->samplesPerPixel) + ").";
        return result;
    }
    const std::uint64_t decodedBytes = static_cast<std::uint64_t>(info->rows) *
                                       static_cast<std::uint64_t>(info->columns) *
                                       static_cast<std::uint64_t>(frames) *
                                       static_cast<std::uint64_t>(info->samplesPerPixel) *
                                       static_cast<std::uint64_t>(std::max(8, info->bitsAllocated) / 8);
    if (decodedBytes > limits.maxPixelBytes) {
        result.status = ParseStatus::Unsupported;
        result.message = "Imagem descomprimida excederia o limite de memória configurado.";
        return result;
    }

    // Truncated files: GDCM silently zero-fills missing native pixel data, so
    // check that the file really contains every declared byte.
    const bool nativeTs = info->transferSyntaxUid.empty() || info->transferSyntaxUid == "1.2.840.10008.1.2" ||
                          info->transferSyntaxUid == "1.2.840.10008.1.2.1" ||
                          info->transferSyntaxUid == "1.2.840.10008.1.2.2";
    if (nativeTs && reachedPixelData) {
        info->pixelDataOffset = stopPos;
        // Native YBR_*_422 stores two samples per pixel (Y Y Cb Cr per pair).
        const bool subsampled422 = info->samplesPerPixel == 3 && (info->photometricInterpretation == "YBR_FULL_422" ||
                                                                  info->photometricInterpretation == "YBR_PARTIAL_422");
        const std::uint64_t samplesOnDisk = subsampled422 ? 2u : static_cast<std::uint64_t>(info->samplesPerPixel);
        const std::uint64_t nativeBytes =
            info->bitsAllocated == 1
                ? (static_cast<std::uint64_t>(info->rows) * info->columns * frames + 7) / 8
                : static_cast<std::uint64_t>(info->rows) * info->columns * frames * samplesOnDisk *
                      (static_cast<std::uint64_t>(info->bitsAllocated) / 8);
        info->pixelDataTruncated = stopPos + nativeBytes > size;
    } else if (nativeTs && !reachedPixelData) {
        info->pixelDataTruncated = true;
    }

    // ---- per-frame attributes ----
    FrameInfo base;
    readPlaneAttributes(a, base.geometry);
    if (!readPixelSpacing(a, base.geometry, SpacingSource::PixelSpacing)) {
        const auto ips = a.dsMulti(0x0018, 0x1164);
        if (ips.size() == 2 && ips[0] > 0.0 && ips[1] > 0.0) {
            base.geometry.spacingY = ips[0];
            base.geometry.spacingX = ips[1];
            base.geometry.spacingSource = SpacingSource::ImagerPixelSpacing;
        } else {
            readUltrasoundCalibration(a, base.geometry);
        }
    }
    if (auto t = a.ds(0x0018, 0x0050)) {
        base.geometry.sliceThickness = t;
    }
    readRescale(a, base);
    readWindows(a, base.windows);
    base.keys.echoNumber = a.is(0x0018, 0x0086);
    base.keys.echoTime = a.ds(0x0018, 0x0081);
    base.keys.temporalPosition = a.is(0x0020, 0x0100);
    base.keys.acquisitionNumber = a.is(0x0020, 0x0012);
    base.keys.triggerTime = a.ds(0x0018, 0x1060);
    base.keys.imageType = imageType;
    if (auto b = a.ds(0x0018, 0x9087)) {
        base.keys.diffusionBValue = b;
    }
    info->voiLut = readVoiLut(a, info->pixelRepresentation == 1);

    const SeqView shared = a.items(0x5200, 0x9229);
    const SeqView perFrame = a.items(0x5200, 0x9230);
    info->isEnhanced = !shared.empty() || !perFrame.empty();
    if (info->isEnhanced) {
        for (const auto* d : shared) {
            applyFunctionalGroup(a.nested(*d), base);
        }
    }

    info->frames.resize(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        FrameInfo f = base;
        f.frameIndex = i;
        if (info->isEnhanced && static_cast<size_t>(i) < perFrame.size()) {
            applyFunctionalGroup(a.nested(*perFrame[static_cast<size_t>(i)]), f);
        }
        info->frames[static_cast<size_t>(i)] = std::move(f);
    }

    // Classic multi-frame (e.g. NM, US cine) shares one plane for all frames;
    // only Enhanced objects describe per-frame positions.
    if (!isTransferSyntaxSupported(info->transferSyntaxUid)) {
        result.status = ParseStatus::Unsupported;
        result.message = "Transfer Syntax não suportada: " + transferSyntaxName(info->transferSyntaxUid) + " (" +
                         info->transferSyntaxUid + "), SOP Class " + info->sopClassUid + ".";
        result.instance = info;
        return result;
    }

    result.status = ParseStatus::Ok;
    result.instance = std::move(info);
    return result;
}

}  // namespace vtc
