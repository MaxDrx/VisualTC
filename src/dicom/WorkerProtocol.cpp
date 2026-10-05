#include "dicom/WorkerProtocol.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace vtc {

// ---------------------------------------------------------------- writer ---

void ByteWriter::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void ByteWriter::u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void ByteWriter::f64(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
}

void ByteWriter::str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
}

void ByteWriter::bytes(const std::vector<std::uint8_t>& b) {
    u64(b.size());
    buf_.insert(buf_.end(), b.begin(), b.end());
}

void ByteWriter::optInt(const std::optional<int>& v) {
    boolean(v.has_value());
    if (v) {
        i32(*v);
    }
}

void ByteWriter::optDouble(const std::optional<double>& v) {
    boolean(v.has_value());
    if (v) {
        f64(*v);
    }
}

// ---------------------------------------------------------------- reader ---

bool ByteReader::u8(std::uint8_t& v) {
    if (remaining() < 1) {
        return false;
    }
    v = p_[off_++];
    return true;
}

bool ByteReader::u32(std::uint32_t& v) {
    if (remaining() < 4) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p_[off_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    off_ += 4;
    return true;
}

bool ByteReader::u64(std::uint64_t& v) {
    if (remaining() < 8) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p_[off_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    off_ += 8;
    return true;
}

bool ByteReader::i32(std::int32_t& v) {
    std::uint32_t u = 0;
    if (!u32(u)) {
        return false;
    }
    v = static_cast<std::int32_t>(u);
    return true;
}

bool ByteReader::f64(double& v) {
    std::uint64_t bits = 0;
    if (!u64(bits)) {
        return false;
    }
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

bool ByteReader::boolean(bool& v) {
    std::uint8_t b = 0;
    if (!u8(b) || b > 1) {
        return false;
    }
    v = b == 1;
    return true;
}

bool ByteReader::str(std::string& s, std::size_t maxLen) {
    std::uint32_t len = 0;
    if (!u32(len) || len > maxLen || len > remaining()) {
        return false;
    }
    s.assign(reinterpret_cast<const char*>(p_ + off_), len);
    off_ += len;
    return true;
}

bool ByteReader::bytes(std::vector<std::uint8_t>& b, std::uint64_t maxLen) {
    std::uint64_t len = 0;
    if (!u64(len) || len > maxLen || len > remaining()) {
        return false;
    }
    b.assign(p_ + off_, p_ + off_ + len);
    off_ += static_cast<std::size_t>(len);
    return true;
}

bool ByteReader::optInt(std::optional<int>& v) {
    bool has = false;
    if (!boolean(has)) {
        return false;
    }
    if (!has) {
        v.reset();
        return true;
    }
    std::int32_t x = 0;
    if (!i32(x)) {
        return false;
    }
    v = x;
    return true;
}

bool ByteReader::optDouble(std::optional<double>& v) {
    bool has = false;
    if (!boolean(has)) {
        return false;
    }
    if (!has) {
        v.reset();
        return true;
    }
    double x = 0.0;
    if (!f64(x)) {
        return false;
    }
    v = x;
    return true;
}

// ------------------------------------------------------------- instances ---

namespace {

void writeVec(ByteWriter& w, const Vec3& v) {
    w.f64(v.x);
    w.f64(v.y);
    w.f64(v.z);
}

bool readVec(ByteReader& r, Vec3& v) { return r.f64(v.x) && r.f64(v.y) && r.f64(v.z); }

void writeFrameInfo(ByteWriter& w, const FrameInfo& f) {
    w.i32(f.frameIndex);
    const auto& g = f.geometry;
    w.boolean(g.hasPosition);
    w.boolean(g.hasOrientation);
    writeVec(w, g.position);
    writeVec(w, g.rowDir);
    writeVec(w, g.colDir);
    w.u8(static_cast<std::uint8_t>(g.spacingSource));
    w.f64(g.spacingX);
    w.f64(g.spacingY);
    w.optDouble(g.sliceThickness);
    w.f64(f.rescaleSlope);
    w.f64(f.rescaleIntercept);
    w.str(f.rescaleType);
    w.u32(static_cast<std::uint32_t>(f.windows.size()));
    for (const auto& win : f.windows) {
        w.f64(win.center);
        w.f64(win.width);
        w.str(win.explanation);
    }
    const auto& k = f.keys;
    w.optInt(k.echoNumber);
    w.optDouble(k.echoTime);
    w.optInt(k.temporalPosition);
    w.optInt(k.acquisitionNumber);
    w.optDouble(k.triggerTime);
    w.optDouble(k.diffusionBValue);
    w.str(k.stackId);
    w.str(k.imageType);
}

bool finiteVec(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool readFrameInfo(ByteReader& r, FrameInfo& f) {
    std::int32_t idx = 0;
    if (!r.i32(idx) || idx < 0) {
        return false;
    }
    f.frameIndex = idx;
    auto& g = f.geometry;
    std::uint8_t src = 0;
    if (!r.boolean(g.hasPosition) || !r.boolean(g.hasOrientation) || !readVec(r, g.position) ||
        !readVec(r, g.rowDir) || !readVec(r, g.colDir) || !r.u8(src) || src > 4 || !r.f64(g.spacingX) ||
        !r.f64(g.spacingY) || !r.optDouble(g.sliceThickness) || !r.f64(f.rescaleSlope) ||
        !r.f64(f.rescaleIntercept) || !r.str(f.rescaleType, 256)) {
        return false;
    }
    g.spacingSource = static_cast<SpacingSource>(src);
    if (!finiteVec(g.position) || !finiteVec(g.rowDir) || !finiteVec(g.colDir) || !std::isfinite(g.spacingX) ||
        !std::isfinite(g.spacingY) || g.spacingX <= 0.0 || g.spacingY <= 0.0 || !std::isfinite(f.rescaleSlope) ||
        !std::isfinite(f.rescaleIntercept)) {
        return false;
    }
    std::uint32_t nw = 0;
    if (!r.u32(nw) || nw > 64) {
        return false;
    }
    f.windows.resize(nw);
    for (auto& win : f.windows) {
        if (!r.f64(win.center) || !r.f64(win.width) || !r.str(win.explanation, 1024) || !std::isfinite(win.center) ||
            !std::isfinite(win.width) || win.width < 1.0) {
            return false;
        }
    }
    auto& k = f.keys;
    return r.optInt(k.echoNumber) && r.optDouble(k.echoTime) && r.optInt(k.temporalPosition) &&
           r.optInt(k.acquisitionNumber) && r.optDouble(k.triggerTime) && r.optDouble(k.diffusionBValue) &&
           r.str(k.stackId, 1024) && r.str(k.imageType, 1024);
}

}  // namespace

void writeInstance(ByteWriter& w, const InstanceInfo& i) {
    w.str(i.filePath);
    w.u64(i.fileSize);
    for (const std::string* s :
         {&i.sopClassUid, &i.sopInstanceUid, &i.transferSyntaxUid, &i.specificCharacterSet, &i.patientName,
          &i.patientId, &i.patientBirthDate, &i.patientSex, &i.patientAge, &i.studyInstanceUid, &i.studyDate,
          &i.studyTime, &i.studyDescription, &i.accessionNumber, &i.institutionName, &i.referringPhysician,
          &i.seriesInstanceUid, &i.seriesDescription, &i.modality, &i.bodyPartExamined, &i.manufacturer,
          &i.manufacturerModel, &i.protocolName, &i.patientPosition, &i.frameOfReferenceUid, &i.seriesDate,
          &i.seriesTime, &i.acquisitionTime, &i.contentTime, &i.photometricInterpretation, &i.voiLutFunction}) {
        w.str(*s);
    }
    w.optInt(i.seriesNumber);
    w.optInt(i.instanceNumber);
    w.optDouble(i.spacingBetweenSlices);
    w.optDouble(i.kvp);
    w.optDouble(i.recommendedFrameRate);
    w.optDouble(i.frameTimeMs);
    for (int v : {i.rows, i.columns, i.samplesPerPixel, i.bitsAllocated, i.bitsStored, i.highBit,
                  i.pixelRepresentation, i.planarConfiguration, i.numberOfFrames}) {
        w.i32(v);
    }
    w.boolean(i.voiLut.has_value());
    if (i.voiLut) {
        w.i32(i.voiLut->firstMapped);
        w.i32(i.voiLut->bitsPerEntry);
        w.str(i.voiLut->explanation);
        w.u32(static_cast<std::uint32_t>(i.voiLut->data.size()));
        for (auto d : i.voiLut->data) {
            w.u8(static_cast<std::uint8_t>(d & 0xFF));
            w.u8(static_cast<std::uint8_t>(d >> 8));
        }
    }
    w.boolean(i.hasPixelData);
    w.boolean(i.isEnhanced);
    w.boolean(i.lossyCompressed);
    w.boolean(i.hasLocalizerImageType);
    w.u64(i.pixelDataOffset);
    w.boolean(i.pixelDataTruncated);
    w.u32(static_cast<std::uint32_t>(i.frames.size()));
    for (const auto& f : i.frames) {
        writeFrameInfo(w, f);
    }
}

bool readInstance(ByteReader& r, InstanceInfo& i, const ParseLimits& limits) {
    if (!r.str(i.filePath, 1u << 16) || !r.u64(i.fileSize)) {
        return false;
    }
    for (std::string* s :
         {&i.sopClassUid, &i.sopInstanceUid, &i.transferSyntaxUid, &i.specificCharacterSet, &i.patientName,
          &i.patientId, &i.patientBirthDate, &i.patientSex, &i.patientAge, &i.studyInstanceUid, &i.studyDate,
          &i.studyTime, &i.studyDescription, &i.accessionNumber, &i.institutionName, &i.referringPhysician,
          &i.seriesInstanceUid, &i.seriesDescription, &i.modality, &i.bodyPartExamined, &i.manufacturer,
          &i.manufacturerModel, &i.protocolName, &i.patientPosition, &i.frameOfReferenceUid, &i.seriesDate,
          &i.seriesTime, &i.acquisitionTime, &i.contentTime, &i.photometricInterpretation, &i.voiLutFunction}) {
        if (!r.str(*s, 1u << 16)) {
            return false;
        }
    }
    if (!r.optInt(i.seriesNumber) || !r.optInt(i.instanceNumber) || !r.optDouble(i.spacingBetweenSlices) ||
        !r.optDouble(i.kvp) || !r.optDouble(i.recommendedFrameRate) || !r.optDouble(i.frameTimeMs)) {
        return false;
    }
    for (int* v : {&i.rows, &i.columns, &i.samplesPerPixel, &i.bitsAllocated, &i.bitsStored, &i.highBit,
                   &i.pixelRepresentation, &i.planarConfiguration, &i.numberOfFrames}) {
        std::int32_t x = 0;
        if (!r.i32(x)) {
            return false;
        }
        *v = x;
    }
    if (i.rows < 0 || i.rows > limits.maxRows || i.columns < 0 || i.columns > limits.maxColumns ||
        i.numberOfFrames < 0 || i.numberOfFrames > limits.maxFrames || i.bitsAllocated < 0 || i.bitsAllocated > 64 ||
        (i.samplesPerPixel != 1 && i.samplesPerPixel != 3)) {
        return false;
    }
    bool hasLut = false;
    if (!r.boolean(hasLut)) {
        return false;
    }
    if (hasLut) {
        VoiLutTable lut;
        std::uint32_t n = 0;
        if (!r.i32(lut.firstMapped) || !r.i32(lut.bitsPerEntry) || !r.str(lut.explanation, 1024) || !r.u32(n) ||
            n > 65536 || lut.bitsPerEntry < 8 || lut.bitsPerEntry > 16) {
            return false;
        }
        lut.data.resize(n);
        for (auto& d : lut.data) {
            std::uint8_t lo = 0;
            std::uint8_t hi = 0;
            if (!r.u8(lo) || !r.u8(hi)) {
                return false;
            }
            d = static_cast<std::uint16_t>(lo | (hi << 8));
        }
        i.voiLut = std::move(lut);
    }
    std::uint32_t nframes = 0;
    if (!r.boolean(i.hasPixelData) || !r.boolean(i.isEnhanced) || !r.boolean(i.lossyCompressed) ||
        !r.boolean(i.hasLocalizerImageType) || !r.u64(i.pixelDataOffset) || !r.boolean(i.pixelDataTruncated) ||
        !r.u32(nframes) || nframes > static_cast<std::uint32_t>(limits.maxFrames)) {
        return false;
    }
    if (i.hasPixelData && static_cast<int>(nframes) != i.numberOfFrames) {
        return false;
    }
    i.frames.resize(nframes);
    for (auto& f : i.frames) {
        if (!readFrameInfo(r, f)) {
            return false;
        }
    }
    return true;
}

// -------------------------------------------------------------- messages ---

std::vector<std::uint8_t> makeRequest(WorkerOp op, const std::string& utf8Path) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(op));
    w.str(utf8Path);
    return std::move(w.buffer());
}

bool readRequest(const std::vector<std::uint8_t>& msg, WorkerOp& op, std::string& utf8Path) {
    ByteReader r(msg.data(), msg.size());
    std::uint8_t o = 0;
    if (!r.u8(o) || !r.str(utf8Path, 1u << 16)) {
        return false;
    }
    op = static_cast<WorkerOp>(o);
    return true;
}

std::vector<std::uint8_t> encodeParseResult(const ParseResult& res) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(res.status));
    w.str(res.message);
    w.boolean(res.instance != nullptr);
    if (res.instance) {
        writeInstance(w, *res.instance);
    }
    return std::move(w.buffer());
}

bool decodeParseResult(const std::vector<std::uint8_t>& msg, ParseResult& res, const ParseLimits& limits) {
    ByteReader r(msg.data(), msg.size());
    std::uint8_t status = 0;
    bool hasInstance = false;
    if (!r.u8(status) || status > static_cast<std::uint8_t>(ParseStatus::Unsupported) || !r.str(res.message, 4096) ||
        !r.boolean(hasInstance)) {
        return false;
    }
    res.status = static_cast<ParseStatus>(status);
    if (hasInstance) {
        auto inst = std::make_shared<InstanceInfo>();
        if (!readInstance(r, *inst, limits)) {
            return false;
        }
        res.instance = std::move(inst);
    } else {
        res.instance.reset();
    }
    return r.remaining() == 0;
}

std::vector<std::uint8_t> encodeDecodeResult(const DecodeResult& res) {
    ByteWriter w;
    w.str(res.error);
    w.u32(static_cast<std::uint32_t>(res.frames.size()));
    for (const auto& f : res.frames) {
        w.i32(f->width);
        w.i32(f->height);
        w.u8(static_cast<std::uint8_t>(f->format));
        w.f64(f->slope);
        w.f64(f->intercept);
        w.boolean(f->monochrome1);
        w.i32(f->bitsStored);
        w.f64(f->minRaw);
        w.f64(f->maxRaw);
        w.bytes(f->data);
    }
    return std::move(w.buffer());
}

bool decodeDecodeResult(const std::vector<std::uint8_t>& msg, DecodeResult& res, const ParseLimits& limits) {
    ByteReader r(msg.data(), msg.size());
    std::uint32_t n = 0;
    if (!r.str(res.error, 4096) || !r.u32(n) || n > static_cast<std::uint32_t>(limits.maxFrames)) {
        return false;
    }
    res.frames.clear();
    res.frames.reserve(n);
    std::uint64_t total = 0;
    for (std::uint32_t k = 0; k < n; ++k) {
        auto f = std::make_shared<DecodedFrame>();
        std::uint8_t fmt = 0;
        if (!r.i32(f->width) || !r.i32(f->height) || !r.u8(fmt) ||
            fmt > static_cast<std::uint8_t>(PixelFormat::RGB8) || !r.f64(f->slope) || !r.f64(f->intercept) ||
            !r.boolean(f->monochrome1) || !r.i32(f->bitsStored) || !r.f64(f->minRaw) || !r.f64(f->maxRaw)) {
            return false;
        }
        f->format = static_cast<PixelFormat>(fmt);
        if (f->width <= 0 || f->height <= 0 || f->width > limits.maxColumns || f->height > limits.maxRows ||
            !std::isfinite(f->slope) || !std::isfinite(f->intercept) || !std::isfinite(f->minRaw) ||
            !std::isfinite(f->maxRaw) || f->minRaw > f->maxRaw || f->bitsStored < 1 || f->bitsStored > 32) {
            return false;
        }
        const std::uint64_t expected = static_cast<std::uint64_t>(f->width) * static_cast<std::uint64_t>(f->height) *
                                       static_cast<std::uint64_t>(bytesPerPixel(f->format));
        total += expected;
        if (total > limits.maxPixelBytes || !r.bytes(f->data, expected) || f->data.size() != expected) {
            return false;
        }
        res.frames.push_back(std::move(f));
    }
    return r.remaining() == 0;
}

}  // namespace vtc
