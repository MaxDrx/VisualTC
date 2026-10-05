// visualtc-worker: isolated DICOM parser/decoder process.
//
// Reads framed requests from stdin and writes framed responses to stdout
// (see dicom/WorkerProtocol.h). It never writes anything else to stdout and
// never logs patient data. If a hostile file crashes a codec, only this
// process dies; the viewer reports the file as unreadable and restarts it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "core/PathUtil.h"
#include "dicom/DicomDecoder.h"
#include "dicom/DicomParser.h"
#include "dicom/WorkerProtocol.h"

namespace {

bool readExact(std::uint8_t* dst, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const std::size_t r = std::fread(dst + got, 1, n - got, stdin);
        if (r == 0) {
            return false;
        }
        got += r;
    }
    return true;
}

bool writeMessage(std::FILE* out, const std::vector<std::uint8_t>& payload) {
    std::uint8_t header[8];
    const std::uint64_t len = payload.size();
    for (int i = 0; i < 8; ++i) {
        header[i] = static_cast<std::uint8_t>(len >> (8 * i));
    }
    if (std::fwrite(header, 1, 8, out) != 8) {
        return false;
    }
    if (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), out) != payload.size()) {
        return false;
    }
    return std::fflush(out) == 0;
}

// The protocol gets a private duplicate of the stdout pipe and file
// descriptor 1 is pointed at stderr: anything a third-party library prints
// on stdout (codec warnings...) can then never corrupt the framed responses.
std::FILE* takeProtocolChannel() {
#if defined(_WIN32)
    const int fd = _dup(_fileno(stdout));
    if (fd < 0) {
        return stdout;
    }
    _setmode(fd, _O_BINARY);
    std::fflush(stdout);
    _dup2(_fileno(stderr), _fileno(stdout));
    std::FILE* f = _fdopen(fd, "wb");
#else
    const int fd = dup(STDOUT_FILENO);
    if (fd < 0) {
        return stdout;
    }
    std::fflush(stdout);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    std::FILE* f = fdopen(fd, "wb");
#endif
    return f != nullptr ? f : stdout;
}

void applyResourceLimits() {
#if !defined(_WIN32)
    // No core dumps: they would contain patient images.
    rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
#if !defined(__SANITIZE_ADDRESS__) && !defined(__APPLE__)
    // Cap the address space so that a corrupted length field turns into a
    // clean bad_alloc instead of exhausting the machine's memory.
    rlimit as{};
    if (getrlimit(RLIMIT_AS, &as) == 0) {
        const rlim_t cap = static_cast<rlim_t>(12ull * 1024ull * 1024ull * 1024ull);
        if (as.rlim_max == RLIM_INFINITY || as.rlim_max > cap) {
            as.rlim_cur = cap;
            setrlimit(RLIMIT_AS, &as);
        }
    }
#endif
#endif
}

}  // namespace

int main() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    applyResourceLimits();
    std::FILE* out = takeProtocolChannel();
    vtc::initializeDicomLibrary();  // silences GDCM's console tracing

    for (;;) {
        std::uint8_t header[8];
        if (!readExact(header, 8)) {
            return 0;  // parent closed the pipe
        }
        std::uint64_t len = 0;
        for (int i = 0; i < 8; ++i) {
            len |= static_cast<std::uint64_t>(header[i]) << (8 * i);
        }
        if (len == 0 || len > (1u << 20)) {
            return 2;  // requests are small: protocol violation
        }
        std::vector<std::uint8_t> msg(static_cast<std::size_t>(len));
        if (!readExact(msg.data(), msg.size())) {
            return 0;
        }
        vtc::WorkerOp op{};
        std::string path;
        if (!vtc::readRequest(msg, op, path)) {
            return 3;
        }
        std::vector<std::uint8_t> response;
        try {
            switch (op) {
                case vtc::WorkerOp::Parse:
                    response = vtc::encodeParseResult(vtc::parseDicomHeader(vtc::utf8ToPath(path)));
                    break;
                case vtc::WorkerOp::Decode: {
                    const auto parsed = vtc::parseDicomHeader(vtc::utf8ToPath(path));
                    vtc::DecodeResult decoded;
                    if (parsed.status != vtc::ParseStatus::Ok || !parsed.instance) {
                        decoded.error = parsed.message.empty() ? "Arquivo não pôde ser lido." : parsed.message;
                    } else {
                        decoded = vtc::decodeInstance(*parsed.instance);
                    }
                    response = vtc::encodeDecodeResult(decoded);
                    break;
                }
                case vtc::WorkerOp::Ping:
                    response = {1};
                    break;
                case vtc::WorkerOp::CrashForTest:
                    std::abort();
                default:
                    return 4;
            }
        } catch (...) {
            vtc::DecodeResult failed;
            failed.error = "Erro inesperado no decodificador.";
            response = op == vtc::WorkerOp::Parse
                           ? vtc::encodeParseResult({vtc::ParseStatus::Malformed, nullptr, failed.error})
                           : vtc::encodeDecodeResult(failed);
        }
        if (!writeMessage(out, response)) {
            return 5;
        }
    }
}
