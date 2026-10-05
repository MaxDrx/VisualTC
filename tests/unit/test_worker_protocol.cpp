#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dicom/DicomDecoder.h"
#include "dicom/DicomParser.h"
#include "dicom/WorkerProtocol.h"
#include "fixtures/DicomFixtures.h"

using namespace vtc;
using namespace vtc::test;

TEST_CASE("Parse results survive the worker protocol round trip", "[protocol]") {
    TempDir dir;
    SyntheticImage img;
    img.rows = 8;
    img.columns = 6;
    img.pixels.assign(48, 1100);
    img.slope = 1.0;
    img.intercept = -1024.0;
    img.windowCenter = 40;
    img.windowWidth = 400;
    img.instanceNumber = 12;
    img.position = {1.5, -2.5, 30.25};
    img.specificCharacterSet = "ISO_IR 100";
    img.patientNameRaw = std::string("JO\xC3O^SILVA");
    REQUIRE(writeDicom(dir.path() / "a", img));
    const auto original = parseDicomHeader(dir.path() / "a");
    REQUIRE(original.status == ParseStatus::Ok);

    const auto bytes = encodeParseResult(original);
    ParseResult copy;
    REQUIRE(decodeParseResult(bytes, copy));
    REQUIRE(copy.status == ParseStatus::Ok);
    const auto& a = *original.instance;
    const auto& b = *copy.instance;
    REQUIRE(b.filePath == a.filePath);
    REQUIRE(b.patientName == a.patientName);
    REQUIRE(b.rows == 8);
    REQUIRE(b.columns == 6);
    REQUIRE(b.instanceNumber == 12);
    REQUIRE(b.frames.size() == 1);
    REQUIRE(b.frames[0].geometry.position.z == Catch::Approx(30.25));
    REQUIRE(b.frames[0].windows.size() == 1);
    REQUIRE(b.frames[0].rescaleIntercept == -1024.0);
    REQUIRE(b.transferSyntaxUid == a.transferSyntaxUid);
}

TEST_CASE("Decode results survive the worker protocol round trip", "[protocol]") {
    TempDir dir;
    SyntheticImage img;
    img.rows = 5;
    img.columns = 7;
    for (int i = 0; i < 35; ++i) {
        img.pixels.push_back(static_cast<std::uint16_t>(1000 + i));
    }
    REQUIRE(writeDicom(dir.path() / "b", img));
    const auto parsed = parseDicomHeader(dir.path() / "b");
    const auto decoded = decodeInstance(*parsed.instance);
    REQUIRE(decoded.ok());
    DecodeResult copy;
    REQUIRE(decodeDecodeResult(encodeDecodeResult(decoded), copy));
    REQUIRE(copy.ok());
    REQUIRE(copy.frames[0]->data == decoded.frames[0]->data);
    REQUIRE(copy.frames[0]->format == decoded.frames[0]->format);
}

TEST_CASE("Malformed or hostile worker messages are rejected", "[protocol][security]") {
    // Truncated message
    DecodeResult d;
    d.frames.push_back(std::make_shared<DecodedFrame>());
    auto f = std::const_pointer_cast<DecodedFrame>(d.frames[0]);
    f->width = 4;
    f->height = 4;
    f->format = PixelFormat::U16;
    f->data.resize(32);
    auto bytes = encodeDecodeResult(d);
    DecodeResult out;
    REQUIRE(decodeDecodeResult(bytes, out));
    bytes.pop_back();
    REQUIRE_FALSE(decodeDecodeResult(bytes, out));

    // Frame whose data length disagrees with its dimensions
    f->data.resize(30);
    REQUIRE_FALSE(decodeDecodeResult(encodeDecodeResult(d), out));

    // Absurd dimensions are refused before any allocation
    f->width = 100000;
    f->data.resize(32);
    REQUIRE_FALSE(decodeDecodeResult(encodeDecodeResult(d), out));

    // Garbage
    std::vector<std::uint8_t> junk(100, 0xFF);
    ParseResult pr;
    REQUIRE_FALSE(decodeParseResult(junk, pr));
    REQUIRE_FALSE(decodeDecodeResult(junk, out));
}

TEST_CASE("Requests encode operation and path", "[protocol]") {
    const auto msg = makeRequest(WorkerOp::Decode, "/dados/JOÃO/IM0001");
    WorkerOp op{};
    std::string path;
    REQUIRE(readRequest(msg, op, path));
    REQUIRE(op == WorkerOp::Decode);
    REQUIRE(path == "/dados/JOÃO/IM0001");
}
