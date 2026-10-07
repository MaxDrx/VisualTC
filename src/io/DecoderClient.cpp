#include "io/DecoderClient.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QProcess>
#include <atomic>
#include <memory>
#include <mutex>

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "dicom/WorkerProtocol.h"

namespace vtc {

namespace {

std::atomic<bool> g_isolation{false};
QString g_workerPath;
std::mutex g_pathMutex;

constexpr int kParseTimeoutMs = 30'000;
constexpr int kDecodeTimeoutMs = 180'000;
constexpr int kExtractTimeoutMs = 4 * 60 * 60 * 1000;  // large exams on slow disks; cancellable

const char* kCrashMessage =
    "O decodificador isolado encerrou inesperadamente ao processar este arquivo; ele pode estar corrompido ou "
    "conter dados inválidos.";
const char* kTimeoutMessage = "Tempo esgotado ao decodificar este arquivo.";

class Worker {
public:
    ~Worker() { stop(); }

    void stop() {
        if (proc_) {
            proc_->closeWriteChannel();
            if (!proc_->waitForFinished(500)) {
                proc_->kill();
                proc_->waitForFinished(1000);
            }
            proc_.reset();
        }
    }

    // Sends one request and returns the response payload, or nullopt when
    // the worker died, timed out or violated the protocol.
    std::optional<std::vector<std::uint8_t>> roundTrip(const std::vector<std::uint8_t>& request, int timeoutMs,
                                                       bool* timedOut, const std::atomic<bool>* cancel = nullptr) {
        cancel_ = cancel;
        if (!ensureStarted()) {
            return std::nullopt;
        }
        QByteArray frame(8, '\0');
        const std::uint64_t len = request.size();
        for (int i = 0; i < 8; ++i) {
            frame[i] = static_cast<char>(len >> (8 * i));
        }
        frame.append(reinterpret_cast<const char*>(request.data()), static_cast<qsizetype>(request.size()));
        if (proc_->write(frame) != frame.size()) {
            fail();
            return std::nullopt;
        }
        QDeadlineTimer deadline(timeoutMs);
        if (!proc_->waitForBytesWritten(static_cast<int>(deadline.remainingTime()))) {
            *timedOut = deadline.hasExpired();
            fail();
            return std::nullopt;
        }
        QByteArray header;
        if (!readExact(header, 8, deadline, timedOut)) {
            fail();
            return std::nullopt;
        }
        std::uint64_t payloadLen = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLen |= static_cast<std::uint64_t>(static_cast<unsigned char>(header[i])) << (8 * i);
        }
        if (payloadLen > kMaxWorkerMessage) {
            logError("decoder", "Resposta do decodificador excede o tamanho máximo; processo descartado.");
            fail();
            return std::nullopt;
        }
        QByteArray payload;
        if (!readExact(payload, static_cast<qint64>(payloadLen), deadline, timedOut)) {
            fail();
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(payload.begin(), payload.end());
    }

    // Tests: the worker ends without this client noticing (as when the
    // system kills an idle process).
    void killSilently() {
        if (proc_) {
            proc_->kill();
        }
    }

private:
    bool ensureStarted() {
        // Without an event loop, QProcess only learns that the worker ended
        // when asked: a zero wait collects that news before it is reused.
        if (proc_ && proc_->state() == QProcess::Running && !proc_->waitForFinished(0)) {
            return true;
        }
        proc_.reset();
        QString path;
        {
            std::lock_guard lock(g_pathMutex);
            path = g_workerPath;
        }
        proc_ = std::make_unique<QProcess>();
        proc_->setProgram(path);
        proc_->setProcessChannelMode(QProcess::SeparateChannels);
        proc_->setStandardErrorFile(QProcess::nullDevice());
        proc_->setReadChannel(QProcess::StandardOutput);
        proc_->start(QIODevice::ReadWrite | QIODevice::Unbuffered);
        if (!proc_->waitForStarted(20'000)) {
            logError("decoder", "Não foi possível iniciar o decodificador isolado: " +
                                    proc_->errorString().toStdString());
            proc_.reset();
            return false;
        }
        return true;
    }

    bool readExact(QByteArray& out, qint64 n, const QDeadlineTimer& deadline, bool* timedOut) {
        out.clear();
        out.reserve(n);
        while (out.size() < n) {
            if (proc_->bytesAvailable() == 0) {
                if (proc_->state() != QProcess::Running && proc_->bytesAvailable() == 0) {
                    return false;
                }
                if (cancel_ != nullptr && cancel_->load()) {
                    return false;  // the caller gave up: the worker is killed by fail()
                }
                // Wait in short slices so that a cancellation is noticed quickly.
                const qint64 slice = cancel_ != nullptr ? std::min<qint64>(250, deadline.remainingTime())
                                                        : deadline.remainingTime();
                if (!proc_->waitForReadyRead(static_cast<int>(std::max<qint64>(1, slice)))) {
                    if (deadline.hasExpired()) {
                        *timedOut = true;
                        return false;
                    }
                    if (proc_->state() != QProcess::Running && proc_->bytesAvailable() == 0) {
                        return false;
                    }
                    continue;
                }
            }
            out.append(proc_->read(n - out.size()));
        }
        return true;
    }

    void fail() {
        if (proc_) {
            proc_->kill();
            proc_->waitForFinished(2000);
            proc_.reset();
        }
    }

    std::unique_ptr<QProcess> proc_;
    const std::atomic<bool>* cancel_ = nullptr;
};

thread_local std::unique_ptr<Worker> t_worker;

Worker& threadWorker() {
    if (!t_worker) {
        t_worker = std::make_unique<Worker>();
    }
    return *t_worker;
}

}  // namespace

void DecoderClient::initialize(bool isolationEnabled, const QString& workerExecutable) {
    QString exe = workerExecutable;
    if (exe.isEmpty()) {
        exe = QCoreApplication::applicationDirPath() + "/visualtc-worker";
#if defined(Q_OS_WIN)
        exe += ".exe";
#endif
    }
    const QFileInfo fi(exe);
    {
        std::lock_guard lock(g_pathMutex);
        g_workerPath = exe;
    }
    const bool available = fi.exists() && fi.isExecutable();
    g_isolation = isolationEnabled && available;
    if (isolationEnabled && !available) {
        logWarning("decoder", "Decodificador isolado não encontrado; usando decodificação no próprio processo.");
    }
    logInfo("decoder", g_isolation ? "Decodificação isolada em processo separado: ativa."
                                   : "Decodificação isolada em processo separado: inativa.");
}

bool DecoderClient::isolationActive() { return g_isolation; }

QString DecoderClient::workerPath() {
    std::lock_guard lock(g_pathMutex);
    return g_workerPath;
}

ParseResult DecoderClient::parse(const std::filesystem::path& file) {
    if (!g_isolation) {
        return parseDicomHeader(file);
    }
    // Cheap rejection of non-DICOM files without a process round trip.
    if (!looksLikeDicom(file)) {
        ParseResult r;
        r.status = ParseStatus::NotDicom;
        return r;
    }
    bool timedOut = false;
    const auto request = makeRequest(WorkerOp::Parse, pathToUtf8(file));
    auto response = threadWorker().roundTrip(request, kParseTimeoutMs, &timedOut);
    if (!response && !timedOut) {
        // A worker that could not start or had ended (a busy or sleeping
        // computer): one more try with a fresh process. A file that really
        // crashes the decoder fails again and is reported.
        logWarning("decoder", "Decodificador isolado indisponível; nova tentativa com outro processo.");
        response = threadWorker().roundTrip(request, kParseTimeoutMs, &timedOut);
    }
    ParseResult r;
    if (!response || !decodeParseResult(*response, r)) {
        r = {};
        r.status = ParseStatus::Malformed;
        r.message = timedOut ? kTimeoutMessage : kCrashMessage;
        logWarning("decoder", "Falha do decodificador isolado durante leitura de cabeçalho.");
    }
    return r;
}

DecodeResult DecoderClient::decode(const std::string& utf8Path) {
    if (!g_isolation) {
        const auto parsed = parseDicomHeader(utf8ToPath(utf8Path));
        DecodeResult d;
        if (parsed.status != ParseStatus::Ok || !parsed.instance) {
            d.error = parsed.message.empty() ? "Arquivo não pôde ser lido." : parsed.message;
            return d;
        }
        return decodeInstance(*parsed.instance);
    }
    bool timedOut = false;
    const auto request = makeRequest(WorkerOp::Decode, utf8Path);
    auto response = threadWorker().roundTrip(request, kDecodeTimeoutMs, &timedOut);
    if (!response && !timedOut) {
        logWarning("decoder", "Decodificador isolado indisponível; nova tentativa com outro processo.");
        response = threadWorker().roundTrip(request, kDecodeTimeoutMs, &timedOut);
    }
    DecodeResult d;
    if (!response || !decodeDecodeResult(*response, d)) {
        d = {};
        d.error = timedOut ? kTimeoutMessage : kCrashMessage;
        logWarning("decoder", "Falha do decodificador isolado durante decodificação de pixels.");
    }
    return d;
}

ExtractResult DecoderClient::extract(const ExtractRequest& request, const std::atomic<bool>* cancel) {
    if (!g_isolation) {
        return extractArchive(utf8ToPath(request.archive), utf8ToPath(request.destDir), request.displayPrefix,
                              request.password, {}, cancel);
    }
    bool timedOut = false;
    const auto response = threadWorker().roundTrip(makeExtractRequest(request), kExtractTimeoutMs, &timedOut, cancel);
    ExtractResult r;
    if (cancel != nullptr && cancel->load()) {
        r.status = ExtractStatus::Cancelled;
        r.message = "Extração cancelada.";
        return r;
    }
    if (!response || !decodeExtractResult(*response, r, request.destDir)) {
        r = {};
        r.status = ExtractStatus::Corrupt;
        r.message = timedOut ? std::string("Tempo esgotado ao extrair o arquivo compactado.")
                             : std::string("O extrator isolado encerrou inesperadamente; o arquivo compactado pode "
                                           "estar danificado.");
        logWarning("decoder", "Falha do processo isolado durante a extração de arquivo compactado.");
    }
    return r;
}

void DecoderClient::releaseThreadWorker() { t_worker.reset(); }

void DecoderClient::endWorkerSilentlyForTest() { threadWorker().killSilently(); }

bool DecoderClient::crashWorkerForTest() {
    bool timedOut = false;
    const auto response = threadWorker().roundTrip(makeRequest(WorkerOp::CrashForTest, ""), 5000, &timedOut);
    return !response.has_value();
}

}  // namespace vtc
