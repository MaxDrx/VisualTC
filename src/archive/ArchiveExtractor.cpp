#include "archive/ArchiveExtractor.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>

#include "core/PathUtil.h"
#include "dicom/DicomParser.h"

namespace vtc {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kSniffBytes = 512;           // enough for DICOM (132) and tar (262) signatures
constexpr std::size_t kChunk = 1u << 20;           // copy buffer
constexpr std::uint64_t kSpaceCheckEvery = 64ull << 20;
constexpr std::size_t kIsoMagicOffset = 32769;     // "CD001" of the primary volume descriptor

bool startsWith(const unsigned char* d, std::size_t n, std::initializer_list<unsigned char> sig,
                std::size_t offset = 0) {
    if (n < offset + sig.size()) {
        return false;
    }
    std::size_t i = offset;
    for (unsigned char c : sig) {
        if (d[i++] != c) {
            return false;
        }
    }
    return true;
}

// Valid UTF-8 is kept; other bytes (names written with an old code page)
// are mapped as Latin-1 so the display name is always valid UTF-8.
std::string sanitizeName(const char* raw) {
    if (raw == nullptr) {
        return {};
    }
    std::string out;
    const auto* p = reinterpret_cast<const unsigned char*>(raw);
    const std::size_t n = std::strlen(raw);
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c >> 5) == 0x6) {
            len = 2;
        } else if ((c >> 4) == 0xE) {
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            len = 4;
        }
        bool valid = len > 0 && i + len <= n;
        for (std::size_t k = 1; valid && k < len; ++k) {
            valid = (p[i + k] >> 6) == 0x2;
        }
        if (valid) {
            if (c >= 0x20 || c == '\t') {
                out.append(raw + i, len);
            }
            i += len;
        } else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            ++i;
        }
    }
    return out;
}

struct ArchiveDeleter {
    void operator()(archive* a) const {
        if (a != nullptr) {
            archive_read_free(a);
        }
    }
};
using ArchivePtr = std::unique_ptr<archive, ArchiveDeleter>;

struct Context {
    const ExtractLimits& limits;
    const std::atomic<bool>* cancel = nullptr;
    fs::path root;
    std::uint64_t archiveBytes = 1;
    std::uint64_t counter = 0;
    std::uint64_t sinceSpaceCheck = 0;
    std::uint64_t skippedTooLarge = 0;
    std::uint64_t corruptEntries = 0;
    ExtractResult& result;

    [[nodiscard]] bool cancelled() const { return cancel != nullptr && cancel->load(); }

    // Accounts for `n` new bytes; returns a non-Ok status when a limit is hit.
    ExtractStatus account(std::uint64_t n) {
        result.bytesWritten += n;
        sinceSpaceCheck += n;
        if (result.bytesWritten > limits.maxTotalBytes) {
            return ExtractStatus::LimitExceeded;
        }
        if (result.bytesWritten > limits.ratioCheckAfter &&
            static_cast<double>(result.bytesWritten) > limits.maxRatio * static_cast<double>(archiveBytes)) {
            return ExtractStatus::LimitExceeded;
        }
        if (sinceSpaceCheck >= kSpaceCheckEvery) {
            sinceSpaceCheck = 0;
            if (!enoughSpace()) {
                return ExtractStatus::DiskFull;
            }
        }
        return ExtractStatus::Ok;
    }

    [[nodiscard]] bool enoughSpace() const {
        std::error_code ec;
        const auto info = fs::space(root, ec);
        return ec || info.available >= limits.minFreeBytes;
    }

    fs::path nextName(const fs::path& dir, const char* prefix = "") {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%06llu", prefix, static_cast<unsigned long long>(++counter));
        return dir / buf;
    }
};

std::string message(ExtractStatus s, const std::string& name) {
    switch (s) {
        case ExtractStatus::Ok: return {};
        case ExtractStatus::NeedsPassword: return "O arquivo compactado " + name + " está protegido por senha.";
        case ExtractStatus::WrongPassword: return "Senha incorreta para o arquivo compactado " + name + ".";
        case ExtractStatus::Unsupported:
            return "O arquivo compactado " + name +
                   " usa um recurso não suportado (por exemplo, 7z ou RAR protegido por senha, ou arquivo "
                   "dividido em partes). Extraia-o com o programa de compactação e abra a pasta.";
        case ExtractStatus::Corrupt: return "O arquivo compactado " + name + " está danificado ou incompleto.";
        case ExtractStatus::LimitExceeded:
            return "O conteúdo de " + name +
                   " excede os limites de segurança de extração (tamanho ou taxa de compressão suspeitos).";
        case ExtractStatus::DiskFull: return "Espaço em disco insuficiente para extrair " + name + ".";
        case ExtractStatus::IoError: return "Não foi possível gravar os arquivos extraídos de " + name + ".";
        case ExtractStatus::Cancelled: return "Extração de " + name + " cancelada.";
    }
    return {};
}

bool isPasswordError(archive* a) {
    const char* e = archive_error_string(a);
    if (e == nullptr) {
        return false;
    }
    const std::string s(e);
    return s.find("assphrase") != std::string::npos;
}

ExtractStatus extractInto(Context& ctx, const fs::path& file, ArchiveKind kind, const fs::path& dir,
                          const std::string& prefix, const std::string& password, int depth);

// Copies the current member to `out`, starting with the already read `head`.
// `readError`: the archive data could not be read (damaged or wrong
// password); `tooLarge`: this member alone exceeds the per-file limit.
ExtractStatus copyMember(Context& ctx, archive* a, const std::vector<unsigned char>& head, const fs::path& out,
                         bool& readError, bool& tooLarge, la_ssize_t& lastRc) {
    readError = false;
    tooLarge = false;
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) {
        return ExtractStatus::IoError;
    }
    std::uint64_t memberBytes = head.size();
    f.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    if (const auto st = ctx.account(head.size()); st != ExtractStatus::Ok) {
        return st;
    }
    std::vector<unsigned char> buf(kChunk);
    for (;;) {
        if (ctx.cancelled()) {
            return ExtractStatus::Cancelled;
        }
        const la_ssize_t n = archive_read_data(a, buf.data(), buf.size());
        if (n == 0) {
            break;
        }
        if (n < 0) {
            readError = true;
            lastRc = n;
            return ExtractStatus::Corrupt;
        }
        memberBytes += static_cast<std::uint64_t>(n);
        if (memberBytes > ctx.limits.maxEntryBytes) {
            tooLarge = true;
            return ExtractStatus::LimitExceeded;
        }
        f.write(reinterpret_cast<const char*>(buf.data()), n);
        if (!f) {
            return ctx.enoughSpace() ? ExtractStatus::IoError : ExtractStatus::DiskFull;
        }
        if (const auto st = ctx.account(static_cast<std::uint64_t>(n)); st != ExtractStatus::Ok) {
            return st;
        }
    }
    f.close();
    return f ? ExtractStatus::Ok : ExtractStatus::IoError;
}

ExtractStatus passwordStatus(ArchiveKind kind, const std::string& password) {
    if (kind != ArchiveKind::Zip) {
        return ExtractStatus::Unsupported;  // libarchive decrypts ZIP only
    }
    return password.empty() ? ExtractStatus::NeedsPassword : ExtractStatus::WrongPassword;
}

ExtractStatus extractInto(Context& ctx, const fs::path& file, ArchiveKind kind, const fs::path& dir,
                          const std::string& prefix, const std::string& password, int depth) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return ExtractStatus::IoError;
    }
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);  // patient images: private

    ArchivePtr a(archive_read_new());
    if (!a) {
        return ExtractStatus::IoError;
    }
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());
    const bool singleStream = kind == ArchiveKind::Gzip || kind == ArchiveKind::Bzip2 || kind == ArchiveKind::Xz ||
                              kind == ArchiveKind::Zstd;
    if (singleStream) {
        // "exame.dcm.gz": a compressed stream that is not a tar archive.
        archive_read_support_format_raw(a.get());
    }
    if (!password.empty()) {
        archive_read_add_passphrase(a.get(), password.c_str());
    }
#if defined(_WIN32)
    const int openRc = archive_read_open_filename_w(a.get(), file.c_str(), 64 * 1024);
#else
    const int openRc = archive_read_open_filename(a.get(), file.c_str(), 64 * 1024);
#endif
    if (openRc != ARCHIVE_OK) {
        return isPasswordError(a.get()) ? passwordStatus(kind, password) : ExtractStatus::Corrupt;
    }

    ExtractStatus worst = ExtractStatus::Ok;
    for (;;) {
        if (ctx.cancelled()) {
            return ExtractStatus::Cancelled;
        }
        archive_entry* entry = nullptr;
        const int rc = archive_read_next_header(a.get(), &entry);
        if (rc == ARCHIVE_EOF) {
            break;
        }
        if (rc == ARCHIVE_FATAL || entry == nullptr) {
            if (isPasswordError(a.get())) {
                return passwordStatus(kind, password);  // encrypted headers (e.g. 7z with hidden names)
            }
            ++ctx.corruptEntries;
            return ExtractStatus::Corrupt;  // files extracted so far are kept
        }
        if (rc == ARCHIVE_FAILED) {
            ++ctx.corruptEntries;
            worst = ExtractStatus::Corrupt;
            continue;  // this member is unreadable; the next one may be fine
        }
        if (++ctx.result.entries > ctx.limits.maxEntries) {
            return ExtractStatus::LimitExceeded;
        }
        // Only plain files; links, directories and devices are never created.
        if (archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) != nullptr ||
            archive_entry_symlink(entry) != nullptr) {
            continue;
        }
        if (archive_entry_size_is_set(entry) != 0 &&
            static_cast<std::uint64_t>(archive_entry_size(entry)) > ctx.limits.maxEntryBytes) {
            ++ctx.skippedTooLarge;
            continue;
        }
        const char* rawName = archive_entry_pathname_utf8(entry);
        std::string name = sanitizeName(rawName != nullptr ? rawName : archive_entry_pathname(entry));
        if (singleStream && (name.empty() || name == "data")) {
            name = pathToUtf8(file.stem());  // raw compressed stream: the archive's own name
        }
        const bool encrypted = archive_entry_is_encrypted(entry) != 0;
        if (encrypted && kind != ArchiveKind::Zip) {
            return ExtractStatus::Unsupported;
        }

        // Classify the member by its first bytes.
        std::vector<unsigned char> head;
        head.reserve(kSniffBytes);
        la_ssize_t lastRc = 0;
        while (head.size() < kSniffBytes) {
            unsigned char tmp[kSniffBytes];
            const la_ssize_t n = archive_read_data(a.get(), tmp, kSniffBytes - head.size());
            if (n <= 0) {
                lastRc = n;
                break;
            }
            head.insert(head.end(), tmp, tmp + n);
        }
        if (lastRc < 0) {
            if (encrypted || isPasswordError(a.get())) {
                return passwordStatus(kind, password);
            }
            ++ctx.corruptEntries;
            if (lastRc == ARCHIVE_FATAL) {
                return ExtractStatus::Corrupt;
            }
            worst = ExtractStatus::Corrupt;
            continue;
        }
        if (head.empty()) {
            continue;
        }
        const bool dicom = looksLikeDicomBytes(head.data(), head.size());
        const ArchiveKind nested = dicom ? ArchiveKind::None : detectArchiveBytes(head.data(), head.size());
        if (!dicom && (nested == ArchiveKind::None || depth >= ctx.limits.maxDepth)) {
            ++ctx.result.skippedNonDicom;
            continue;  // the rest of the member is skipped by the next header read
        }
        if (!ctx.enoughSpace()) {
            return ExtractStatus::DiskFull;
        }
        const fs::path out = ctx.nextName(dir, dicom ? "" : "z");
        bool readError = false;
        bool tooLarge = false;
        const ExtractStatus st = copyMember(ctx, a.get(), head, out, readError, tooLarge, lastRc);
        if (st != ExtractStatus::Ok) {
            fs::remove(out, ec);
            if (tooLarge) {
                ++ctx.skippedTooLarge;
                continue;
            }
            if (readError) {
                if (encrypted || isPasswordError(a.get())) {
                    return passwordStatus(kind, password);
                }
                ++ctx.corruptEntries;
                if (lastRc == ARCHIVE_FATAL) {
                    return ExtractStatus::Corrupt;
                }
                worst = ExtractStatus::Corrupt;
                continue;
            }
            return st;
        }
        if (dicom) {
            ctx.result.files.push_back({pathToUtf8(out), prefix + name});
            continue;
        }
        // Archive inside the archive: expand it next to the others, then drop
        // the intermediate copy.
        fs::path sub = out;
        sub += "-conteudo";
        const ExtractStatus inner = extractInto(ctx, out, nested, sub, prefix + name + " › ", password, depth + 1);
        fs::remove(out, ec);
        if (inner == ExtractStatus::Cancelled || inner == ExtractStatus::DiskFull ||
            inner == ExtractStatus::LimitExceeded || inner == ExtractStatus::IoError) {
            return inner;
        }
        if (inner != ExtractStatus::Ok && worst == ExtractStatus::Ok) {
            worst = inner;  // keep going with the other members
        }
    }
    return worst;
}

}  // namespace

ArchiveKind detectArchiveBytes(const unsigned char* d, std::size_t n) {
    if (d == nullptr) {
        return ArchiveKind::None;
    }
    if (startsWith(d, n, {'P', 'K', 0x03, 0x04}) || startsWith(d, n, {'P', 'K', 0x05, 0x06}) ||
        startsWith(d, n, {'P', 'K', 0x07, 0x08})) {
        return ArchiveKind::Zip;
    }
    if (startsWith(d, n, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C})) {
        return ArchiveKind::SevenZip;
    }
    if (startsWith(d, n, {'R', 'a', 'r', '!', 0x1A, 0x07})) {
        return ArchiveKind::Rar;
    }
    if (startsWith(d, n, {0x1F, 0x8B})) {
        return ArchiveKind::Gzip;
    }
    if (startsWith(d, n, {'B', 'Z', 'h'}) && n > 3 && d[3] >= '1' && d[3] <= '9') {
        return ArchiveKind::Bzip2;
    }
    if (startsWith(d, n, {0xFD, '7', 'z', 'X', 'Z', 0x00})) {
        return ArchiveKind::Xz;
    }
    if (startsWith(d, n, {0x28, 0xB5, 0x2F, 0xFD})) {
        return ArchiveKind::Zstd;
    }
    if (startsWith(d, n, {'u', 's', 't', 'a', 'r'}, 257)) {
        return ArchiveKind::Tar;
    }
    if (startsWith(d, n, {'C', 'D', '0', '0', '1'}, kIsoMagicOffset)) {
        return ArchiveKind::Iso9660;
    }
    return ArchiveKind::None;
}

ArchiveKind detectArchive(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return ArchiveKind::None;
    }
    std::vector<unsigned char> buf(kIsoMagicOffset + 5);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto got = static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount()));
    return detectArchiveBytes(buf.data(), got);
}

std::string archiveKindName(ArchiveKind kind) {
    switch (kind) {
        case ArchiveKind::None: return {};
        case ArchiveKind::Zip: return "ZIP";
        case ArchiveKind::SevenZip: return "7z";
        case ArchiveKind::Rar: return "RAR";
        case ArchiveKind::Tar: return "TAR";
        case ArchiveKind::Gzip: return "GZIP";
        case ArchiveKind::Bzip2: return "BZIP2";
        case ArchiveKind::Xz: return "XZ";
        case ArchiveKind::Zstd: return "Zstandard";
        case ArchiveKind::Iso9660: return "ISO 9660";
    }
    return {};
}

ExtractResult extractArchive(const fs::path& file, const fs::path& destDir, const std::string& displayPrefix,
                             const std::string& password, const ExtractLimits& limits,
                             const std::atomic<bool>* cancel) {
    ExtractResult result;
    const std::string name = pathToUtf8(file.filename());
    const ArchiveKind kind = detectArchive(file);
    if (kind == ArchiveKind::None) {
        result.status = ExtractStatus::Corrupt;
        result.message = "O arquivo " + name + " não é um arquivo compactado reconhecido.";
        return result;
    }
    Context ctx{limits, cancel, destDir, 1, 0, 0, 0, 0, result};
    std::error_code ec;
    ctx.archiveBytes = std::max<std::uint64_t>(1, fs::file_size(file, ec));
    fs::create_directories(destDir, ec);
    if (ec) {
        result.status = ExtractStatus::IoError;
        result.message = message(result.status, name);
        return result;
    }
    if (!ctx.enoughSpace()) {
        result.status = ExtractStatus::DiskFull;
        result.message = message(result.status, name);
        return result;
    }
    try {
        result.status = extractInto(ctx, file, kind, destDir, displayPrefix, password, 1);
    } catch (const std::bad_alloc&) {
        result.status = ExtractStatus::LimitExceeded;
    } catch (...) {
        result.status = ExtractStatus::Corrupt;
    }
    result.message = message(result.status, name);
    if (result.status == ExtractStatus::Ok && ctx.skippedTooLarge > 0) {
        result.message = std::to_string(ctx.skippedTooLarge) + " membro(s) de " + name +
                         " ignorado(s) por excederem o tamanho máximo por arquivo.";
    }
    return result;
}

std::string libarchiveVersion() { return archive_version_details(); }

}  // namespace vtc
