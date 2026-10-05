#pragma once

#include <QObject>
#include <QStringList>
#include <atomic>
#include <memory>
#include <thread>

#include "dicom/DicomScanner.h"

namespace vtc {

// Background folder/file import: walks the paths, reads headers through the
// isolated decoder and reports progress. The GUI stays responsive.
class ImportTask : public QObject {
    Q_OBJECT
public:
    explicit ImportTask(QObject* parent = nullptr);
    ~ImportTask() override;

    bool start(const QStringList& paths);
    void cancel();
    [[nodiscard]] bool running() const { return running_; }
    // Valid after finished() was emitted.
    [[nodiscard]] const ScanResult& result() const { return result_; }

Q_SIGNALS:
    void progress(int processed, int total, int imagesFound);
    void finished();

private:
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    ScanResult result_;
};

}  // namespace vtc
