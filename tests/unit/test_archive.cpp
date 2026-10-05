// Compressed exams: detection, extraction of every supported format, path
// safety, passwords, decompression bombs and damaged archives.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <fstream>
#include <random>
#include <set>

#include "archive/ArchiveExtractor.h"
#include "core/PathUtil.h"
#include "dicom/DicomParser.h"
#include "dicom/DicomScanner.h"
#include "dicom/WorkerProtocol.h"
#include "fixtures/DicomFixtures.h"

using namespace vtc;
using namespace vtc::test;
namespace fs = std::filesystem;

namespace {

// Bytes of a small, valid CT slice.
std::vector<std::uint8_t> dicomBytes(const fs::path& scratch, int index) {
    SyntheticImage img;
    img.rows = img.columns = 16;
    img.position = {0.0, 0.0, index * 2.0};
    img.instanceNumber = index + 1;
    img.seriesInstanceUid = "1.2.826.0.1.3680043.10.999.777";
    img.slope = 1.0;
    img.intercept = -1024.0;
    img.pixels.assign(16 * 16, static_cast<std::uint16_t>(1024 + index));
    const auto file = scratch / ("slice" + std::to_string(index));
    REQUIRE(writeDicom(file, img));
    return readBytes(file);
}

std::vector<std::uint8_t> text(const std::string& s) { return {s.begin(), s.end()}; }

// Every file below `root` (recursively).
std::set<std::string> listFiles(const fs::path& root) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() || it->is_symlink()) {
            out.insert(pathToUtf8(it->path()));
        }
    }
    return out;
}

bool isInside(const fs::path& file, const fs::path& root) {
    const auto rel = fs::weakly_canonical(file).lexically_relative(fs::weakly_canonical(root));
    return !rel.empty() && *rel.begin() != "..";
}

}  // namespace

TEST_CASE("Archives are recognised by content, not by extension", "[archive]") {
    TempDir dir;
    const auto dcm = dicomBytes(dir.path(), 0);
    REQUIRE(writeArchive(dir.path() / "exame.dat", ArchiveFormat::Zip, {{"IM1", dcm, {}}}));
    REQUIRE(writeArchive(dir.path() / "semextensao", ArchiveFormat::SevenZip, {{"IM1", dcm, {}}}));
    REQUIRE(writeArchive(dir.path() / "a.tgz", ArchiveFormat::TarGz, {{"IM1", dcm, {}}}));
    REQUIRE(writeArchive(dir.path() / "a.tar.xz", ArchiveFormat::TarXz, {{"IM1", dcm, {}}}));
    REQUIRE(writeArchive(dir.path() / "a.iso", ArchiveFormat::Iso9660, {{"IM1", dcm, {}}}));
    REQUIRE(detectArchive(dir.path() / "exame.dat") == ArchiveKind::Zip);
    REQUIRE(detectArchive(dir.path() / "semextensao") == ArchiveKind::SevenZip);
    REQUIRE(detectArchive(dir.path() / "a.tgz") == ArchiveKind::Gzip);
    REQUIRE(detectArchive(dir.path() / "a.tar.xz") == ArchiveKind::Xz);
    REQUIRE(detectArchive(dir.path() / "a.iso") == ArchiveKind::Iso9660);
    // A DICOM file called ".zip" is still DICOM; text is nothing.
    {
        std::ofstream(dir.path() / "imagem.zip", std::ios::binary)
            .write(reinterpret_cast<const char*>(dcm.data()), static_cast<std::streamsize>(dcm.size()));
        std::ofstream(dir.path() / "leiame.zip") << "nao sou um zip";
    }
    REQUIRE(detectArchive(dir.path() / "imagem.zip") == ArchiveKind::None);
    REQUIRE(detectArchive(dir.path() / "leiame.zip") == ArchiveKind::None);
}

TEST_CASE("DICOM members of every supported format are extracted", "[archive]") {
    TempDir dir;
    const auto format = GENERATE(ArchiveFormat::Zip, ArchiveFormat::ZipStored, ArchiveFormat::SevenZip,
                                 ArchiveFormat::TarGz, ArchiveFormat::TarBz2, ArchiveFormat::TarXz,
                                 ArchiveFormat::TarZstd, ArchiveFormat::Iso9660);
    CAPTURE(static_cast<int>(format));
    std::vector<ArchiveMember> members;
    for (int i = 0; i < 3; ++i) {
        members.push_back({"DICOM/ST1/IM" + std::to_string(i + 1), dicomBytes(dir.path(), i), {}});
    }
    members.push_back({"LEIAME.TXT", text("Exame entregue pelo portal."), {}});
    members.push_back({"VIEWER/visualizador.exe", text("MZ............"), {}});
    const auto archive = dir.path() / "exame";
    REQUIRE(writeArchive(archive, format, members));

    const auto dest = dir.path() / "saida";
    const auto r = extractArchive(archive, dest, "exame.zip › ");
    INFO(r.message);
    REQUIRE(r.ok());
    REQUIRE(r.files.size() == 3);
    REQUIRE(r.skippedNonDicom == 2);
    std::set<std::string> names;
    for (const auto& f : r.files) {
        REQUIRE(isInside(utf8ToPath(f.path), dest));
        const auto parsed = parseDicomHeader(utf8ToPath(f.path));
        REQUIRE(parsed.status == ParseStatus::Ok);
        names.insert(f.displayName);
    }
    REQUIRE(names.count("exame.zip › DICOM/ST1/IM1") == 1);
}

TEST_CASE("A single gzip-compressed DICOM file is opened", "[archive]") {
    TempDir dir;
    REQUIRE(writeArchive(dir.path() / "IM0001.dcm.gz", ArchiveFormat::RawGzip, {{"IM0001.dcm", dicomBytes(dir.path(), 0), {}}}));
    const auto r = extractArchive(dir.path() / "IM0001.dcm.gz", dir.path() / "saida", "IM0001.dcm.gz › ");
    REQUIRE(r.ok());
    REQUIRE(r.files.size() == 1);
    REQUIRE(parseDicomHeader(utf8ToPath(r.files[0].path)).status == ParseStatus::Ok);
}

TEST_CASE("Hostile member names and links never escape the destination", "[archive][security]") {
    TempDir dir;
    const auto dcm = dicomBytes(dir.path(), 0);
    const auto format = GENERATE(ArchiveFormat::Zip, ArchiveFormat::TarGz);
    const std::vector<ArchiveMember> members = {
        {"../../fora1.dcm", dcm, {}},
        {"/tmp/visualtc-absoluto.dcm", dcm, {}},
        {"..\\..\\fora2.dcm", dcm, {}},
        {"C:/Windows/fora3.dcm", dcm, {}},
        {"link-para-fora", {}, "../../../../etc/passwd"},
        {"pasta/../../fora4.dcm", dcm, {}},
    };
    const auto archive = dir.path() / "hostil";
    REQUIRE(writeArchive(archive, format, members));
    const auto before = listFiles(dir.path());
    const auto dest = dir.path() / "area" / "saida";
    const auto r = extractArchive(archive, dest, "hostil › ");
    REQUIRE(r.ok());
    REQUIRE(r.files.size() == 5);  // the five regular members, the link is skipped
    for (const auto& f : r.files) {
        REQUIRE(isInside(utf8ToPath(f.path), dest));
    }
    // Nothing appeared anywhere except inside the destination folder.
    for (const auto& f : listFiles(dir.path())) {
        if (before.count(f) == 0) {
            REQUIRE(isInside(utf8ToPath(f), dest));
            REQUIRE_FALSE(fs::is_symlink(utf8ToPath(f)));
        }
    }
    REQUIRE_FALSE(fs::exists("/tmp/visualtc-absoluto.dcm"));
}

TEST_CASE("Archives inside archives are expanded", "[archive]") {
    TempDir dir;
    REQUIRE(writeArchive(dir.path() / "interno.zip", ArchiveFormat::Zip,
                         {{"IM1", dicomBytes(dir.path(), 0), {}}, {"IM2", dicomBytes(dir.path(), 1), {}}}));
    REQUIRE(writeArchive(dir.path() / "serie.tgz", ArchiveFormat::TarGz, {{"IM3", dicomBytes(dir.path(), 2), {}}}));
    REQUIRE(writeArchive(dir.path() / "externo.zip", ArchiveFormat::Zip,
                         {{"interno.zip", readBytes(dir.path() / "interno.zip"), {}},
                          {"sub/serie.tgz", readBytes(dir.path() / "serie.tgz"), {}}}));
    const auto r = extractArchive(dir.path() / "externo.zip", dir.path() / "saida", "externo.zip › ");
    REQUIRE(r.ok());
    REQUIRE(r.files.size() == 3);
    std::set<std::string> names;
    for (const auto& f : r.files) {
        names.insert(f.displayName);
        REQUIRE(isInside(utf8ToPath(f.path), dir.path() / "saida"));
    }
    REQUIRE(names.count("externo.zip › interno.zip › IM1") == 1);
    REQUIRE(names.count("externo.zip › sub/serie.tgz › IM3") == 1);
    // Intermediate copies of the inner archives are not left behind.
    for (const auto& f : listFiles(dir.path() / "saida")) {
        REQUIRE(detectArchive(utf8ToPath(f)) == ArchiveKind::None);
    }
}

TEST_CASE("Password-protected ZIP files ask for the password", "[archive]") {
    TempDir dir;
    const auto encryption = GENERATE(std::string("zipcrypt"), std::string("aes256"));
    CAPTURE(encryption);
    const auto archive = dir.path() / "protegido.zip";
    REQUIRE(writeArchive(archive, ArchiveFormat::Zip,
                         {{"IM1", dicomBytes(dir.path(), 0), {}}, {"IM2", dicomBytes(dir.path(), 1), {}}}, "12/03/1961",
                         encryption));
    const auto none = extractArchive(archive, dir.path() / "a", "protegido.zip › ");
    REQUIRE(none.status == ExtractStatus::NeedsPassword);
    REQUIRE(none.files.empty());
    const auto wrong = extractArchive(archive, dir.path() / "b", "protegido.zip › ", "errada");
    REQUIRE(wrong.status == ExtractStatus::WrongPassword);
    REQUIRE(wrong.files.empty());
    const auto right = extractArchive(archive, dir.path() / "c", "protegido.zip › ", "12/03/1961");
    INFO(right.message);
    REQUIRE(right.ok());
    REQUIRE(right.files.size() == 2);
    REQUIRE(parseDicomHeader(utf8ToPath(right.files[0].path)).status == ParseStatus::Ok);
}

TEST_CASE("Decompression bombs and oversized members are stopped", "[archive][security]") {
    TempDir dir;
    auto big = dicomBytes(dir.path(), 0);
    big.resize(big.size() + (24u << 20), 0);  // DICOM header followed by 24 MiB of zeros
    const auto archive = dir.path() / "bomba.zip";
    REQUIRE(writeArchive(archive, ArchiveFormat::Zip, {{"IM1", big, {}}, {"IM2", dicomBytes(dir.path(), 1), {}}}));
    REQUIRE(fs::file_size(archive) < (1u << 20));

    ExtractLimits total;
    total.maxTotalBytes = 8u << 20;
    const auto a = extractArchive(archive, dir.path() / "a", "bomba.zip › ", {}, total);
    REQUIRE(a.status == ExtractStatus::LimitExceeded);
    REQUIRE(a.bytesWritten <= total.maxTotalBytes + (1u << 20));

    ExtractLimits ratio;
    ratio.ratioCheckAfter = 1u << 20;
    ratio.maxRatio = 5.0;
    REQUIRE(extractArchive(archive, dir.path() / "b", "bomba.zip › ", {}, ratio).status ==
            ExtractStatus::LimitExceeded);

    // One member above the per-file limit is skipped; the others still come.
    ExtractLimits perFile;
    perFile.maxEntryBytes = 4u << 20;
    const auto c = extractArchive(archive, dir.path() / "c", "bomba.zip › ", {}, perFile);
    REQUIRE(c.ok());
    REQUIRE(c.files.size() == 1);
    REQUIRE_FALSE(c.message.empty());
    // No partial copy of the skipped member remains.
    REQUIRE(listFiles(dir.path() / "c").size() == 1);
}

TEST_CASE("Damaged archives are reported and never crash", "[archive][security]") {
    TempDir dir;
    const auto format = GENERATE(ArchiveFormat::Zip, ArchiveFormat::SevenZip, ArchiveFormat::TarGz, ArchiveFormat::TarXz);
    CAPTURE(static_cast<int>(format));
    std::vector<ArchiveMember> members;
    for (int i = 0; i < 4; ++i) {
        members.push_back({"IM" + std::to_string(i), dicomBytes(dir.path(), i), {}});
    }
    REQUIRE(writeArchive(dir.path() / "base", format, members));
    const auto original = readBytes(dir.path() / "base");
    std::mt19937 gen(31);
    for (int trial = 0; trial < 150; ++trial) {
        auto bytes = original;
        if (trial % 5 == 0) {
            bytes.resize(gen() % bytes.size());  // truncated download
        } else {
            for (int k = 0; k < 1 + static_cast<int>(gen() % 6); ++k) {
                bytes[8 + gen() % (bytes.size() - 8)] = static_cast<std::uint8_t>(gen());
            }
        }
        const auto file = dir.path() / ("dano" + std::to_string(trial));
        std::ofstream(file, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        const auto dest = dir.path() / ("d" + std::to_string(trial));
        const auto r = extractArchive(file, dest, "dano › ");
        for (const auto& f : r.files) {
            REQUIRE(isInside(utf8ToPath(f.path), dest));
        }
        if (!r.ok()) {
            REQUIRE_FALSE(r.message.empty());
        }
    }
    SUCCEED("no crash");
}

TEST_CASE("The scanner opens compressed exams found among the inputs", "[archive][io]") {
    TempDir dir;
    const auto exam = dir.path() / "exame";
    fs::create_directories(exam / "pasta");
    // Two loose slices, a ZIP with three more, a ZIP without images.
    for (int i = 0; i < 2; ++i) {
        const auto b = dicomBytes(dir.path(), i);
        std::ofstream(exam / ("solto" + std::to_string(i)), std::ios::binary)
            .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    std::vector<ArchiveMember> members;
    for (int i = 2; i < 5; ++i) {
        members.push_back({"DICOM/IM" + std::to_string(i), dicomBytes(dir.path(), i), {}});
    }
    REQUIRE(writeArchive(exam / "pasta" / "exame.zip", ArchiveFormat::Zip, members));
    REQUIRE(writeArchive(exam / "fotos.zip", ArchiveFormat::Zip, {{"foto.txt", text("sem imagens"), {}}}));

    ScanOptions options;
    options.extractRoot = dir.path() / "extraidos";
    options.expander = [](const fs::path& archive, const fs::path& dest, const std::string& prefix) {
        return extractArchive(archive, dest, prefix);
    };
    DicomScanner scanner(std::move(options));
    const auto result = scanner.scan({exam});
    REQUIRE(result.instances.size() == 5);
    REQUIRE(result.archivesOpened == 2);
    REQUIRE(result.displayNames.size() == 3);
    bool noImagesReported = false;
    for (const auto& issue : result.issues) {
        noImagesReported = noImagesReported || issue.message.find("não contém imagens DICOM") != std::string::npos;
    }
    REQUIRE(noImagesReported);
    // Without an expander, archives are simply not DICOM.
    DicomScanner plain;
    REQUIRE(plain.scan({exam}).instances.size() == 2);
}

TEST_CASE("Extraction messages survive the worker protocol and are validated", "[archive][protocol]") {
    ExtractRequest req{"/dados/exame.zip", "/cache/sessao/importacao-1/arquivo-1", "exame.zip › ", "segredo"};
    const auto msg = makeExtractRequest(req);
    WorkerOp op{};
    std::string path;
    ExtractRequest back;
    REQUIRE(readRequest(msg, op, path, &back));
    REQUIRE(op == WorkerOp::Extract);
    REQUIRE(back.archive == req.archive);
    REQUIRE(back.destDir == req.destDir);
    REQUIRE(back.displayPrefix == req.displayPrefix);
    REQUIRE(back.password == req.password);

    ExtractResult r;
    r.status = ExtractStatus::Ok;
    r.entries = 4;
    r.files.push_back({req.destDir + "/000001", "exame.zip › IM1"});
    ExtractResult decoded;
    REQUIRE(decodeExtractResult(encodeExtractResult(r), decoded, req.destDir));
    REQUIRE(decoded.files.size() == 1);
    REQUIRE(decoded.files[0].displayName == "exame.zip › IM1");
    // A (compromised) worker pointing outside the destination is rejected.
    for (const std::string& evil : {std::string("/etc/passwd"), req.destDir + "/../../outra/000001",
                                   std::string("/cache/sessao/importacao-1/arquivo-10/x"), req.destDir}) {
        ExtractResult bad = r;
        bad.files[0].path = evil;
        ExtractResult out;
        CAPTURE(evil);
        REQUIRE_FALSE(decodeExtractResult(encodeExtractResult(bad), out, req.destDir));
    }
}
