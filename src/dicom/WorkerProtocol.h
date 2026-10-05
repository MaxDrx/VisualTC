#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dicom/DicomDecoder.h"
#include "dicom/DicomParser.h"

namespace vtc {

// Wire protocol between VisualTC and its isolated decoder process
// (visualtc-worker). Untrusted DICOM files are parsed and decoded only in the
// worker, so a crash or memory corruption caused by a hostile file inside a
// third-party codec cannot take down or compromise the viewer.
//
// Framing: [u64 little-endian payload length][payload]. The first payload
// byte is the operation. All readers validate every length against hard
// limits, because a compromised worker must not be able to make the viewer
// allocate or read out of bounds either.
enum class WorkerOp : std::uint8_t { Parse = 1, Decode = 2, Ping = 3, CrashForTest = 99 };

constexpr std::uint64_t kMaxWorkerMessage = 6ull * 1024ull * 1024ull * 1024ull;

class ByteWriter {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f64(double v);
    void boolean(bool v) { u8(v ? 1 : 0); }
    void str(const std::string& s);
    void bytes(const std::vector<std::uint8_t>& b);
    void optInt(const std::optional<int>& v);
    void optDouble(const std::optional<double>& v);
    std::vector<std::uint8_t>& buffer() { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : p_(data), n_(size) {}
    bool u8(std::uint8_t& v);
    bool u32(std::uint32_t& v);
    bool u64(std::uint64_t& v);
    bool i32(std::int32_t& v);
    bool f64(double& v);
    bool boolean(bool& v);
    bool str(std::string& s, std::size_t maxLen = 1u << 20);
    bool bytes(std::vector<std::uint8_t>& b, std::uint64_t maxLen);
    bool optInt(std::optional<int>& v);
    bool optDouble(std::optional<double>& v);
    [[nodiscard]] std::size_t remaining() const { return n_ - off_; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t off_ = 0;
};

// Requests
std::vector<std::uint8_t> makeRequest(WorkerOp op, const std::string& utf8Path);
bool readRequest(const std::vector<std::uint8_t>& msg, WorkerOp& op, std::string& utf8Path);

// Responses
std::vector<std::uint8_t> encodeParseResult(const ParseResult& r);
bool decodeParseResult(const std::vector<std::uint8_t>& msg, ParseResult& r, const ParseLimits& limits = {});
std::vector<std::uint8_t> encodeDecodeResult(const DecodeResult& r);
bool decodeDecodeResult(const std::vector<std::uint8_t>& msg, DecodeResult& r, const ParseLimits& limits = {});

// Instance serialization (also usable for caches).
void writeInstance(ByteWriter& w, const InstanceInfo& i);
bool readInstance(ByteReader& r, InstanceInfo& i, const ParseLimits& limits);

}  // namespace vtc
