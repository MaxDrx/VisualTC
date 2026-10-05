#include "io/ImportTask.h"

#include <QMetaObject>
#include <QPointer>
#include <chrono>
#include <future>

#include "core/PathUtil.h"
#include "io/DecoderClient.h"

namespace vtc {

ImportTask::ImportTask(QObject* parent) : QObject(parent) {}

ImportTask::~ImportTask() {
    cancel();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ImportTask::cancel() { cancel_ = true; }

std::optional<std::string> ImportTask::askPassword(const QString& archiveName, bool retry) {
    if (!passwordProvider_) {
        return std::nullopt;
    }
    // The dialog must run in the GUI thread; this (import) thread waits for
    // the answer but keeps honouring cancellation, so closing the window
    // never deadlocks on a pending question.
    auto answer = std::make_shared<std::promise<std::optional<QString>>>();
    auto future = answer->get_future();
    QPointer<ImportTask> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, answer, archiveName, retry] {
            std::optional<QString> pw;
            if (self && self->passwordProvider_ && !self->cancel_) {
                pw = self->passwordProvider_(archiveName, retry);
            }
            answer->set_value(pw);
        },
        Qt::QueuedConnection);
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        if (cancel_) {
            return std::nullopt;
        }
    }
    const auto pw = future.get();
    if (!pw || pw->isEmpty()) {
        return std::nullopt;
    }
    return pw->toStdString();
}

ExtractResult ImportTask::expand(const std::filesystem::path& archive, const std::filesystem::path& dest,
                                 const std::string& displayPrefix) {
    ExtractRequest request{pathToUtf8(archive), pathToUtf8(dest), displayPrefix, {}};
    ExtractResult r = DecoderClient::extract(request, &cancel_);
    const QString name = QString::fromStdString(pathToUtf8(archive.filename()));
    for (int attempt = 0; attempt < 3 && !cancel_ &&
                          (r.status == ExtractStatus::NeedsPassword || r.status == ExtractStatus::WrongPassword);
         ++attempt) {
        const auto password = askPassword(name, r.status == ExtractStatus::WrongPassword);
        if (!password) {
            break;  // the user cancelled the question: report as protected
        }
        std::error_code ec;
        std::filesystem::remove_all(dest, ec);  // nothing from the failed attempt is kept
        request.password = *password;
        r = DecoderClient::extract(request, &cancel_);
    }
    return r;
}

bool ImportTask::start(const QStringList& paths, const std::filesystem::path& extractRoot) {
    if (running_) {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::vector<std::filesystem::path> inputs;
    for (const auto& p : paths) {
        inputs.push_back(utf8ToPath(p.toStdString()));
    }
    cancel_ = false;
    running_ = true;
    result_ = {};
    QPointer<ImportTask> self(this);
    thread_ = std::thread([this, self, inputs, extractRoot] {
        ScanOptions options;
        options.parser = &DecoderClient::parse;
        if (!extractRoot.empty()) {
            options.extractRoot = extractRoot;
            options.expander = [this](const std::filesystem::path& archive, const std::filesystem::path& dest,
                                      const std::string& prefix) { return expand(archive, dest, prefix); };
        }
        DicomScanner scanner(std::move(options));
        ScanResult r = scanner.scan(
            inputs,
            [this, self](const ScanProgress& p) {
                if (!self) {
                    return;
                }
                const int processed = static_cast<int>(p.filesProcessed);
                const int total = static_cast<int>(p.filesFound);
                const int found = static_cast<int>(p.dicomImages);
                const QString archive = QString::fromStdString(p.archive);
                QMetaObject::invokeMethod(
                    this,
                    [this, processed, total, found, archive] { Q_EMIT progress(processed, total, found, archive); },
                    Qt::QueuedConnection);
            },
            &cancel_);
        DecoderClient::releaseThreadWorker();
        QMetaObject::invokeMethod(
            this,
            [this, r = std::move(r)]() mutable {
                result_ = std::move(r);
                running_ = false;
                Q_EMIT finished();
            },
            Qt::QueuedConnection);
    });
    return true;
}

}  // namespace vtc
