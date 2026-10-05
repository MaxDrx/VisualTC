#include "io/ImportTask.h"

#include <QMetaObject>
#include <QPointer>

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

bool ImportTask::start(const QStringList& paths) {
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
    thread_ = std::thread([this, self, inputs] {
        ScanOptions options;
        options.parser = &DecoderClient::parse;
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
                QMetaObject::invokeMethod(
                    this, [this, processed, total, found] { Q_EMIT progress(processed, total, found); },
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
