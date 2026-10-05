#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include "dicom/DicomScanner.h"

namespace vtc {

// Background folder/file import: walks the paths, reads headers through the
// isolated decoder, expands compressed exams (ZIP, 7z, RAR, TAR, GZ, ISO) and
// reports progress. The GUI stays responsive.
class ImportTask : public QObject {
    Q_OBJECT
public:
    // Asked (in the GUI thread) for the password of an encrypted archive.
    // `retry` is true after a wrong password. Empty optional: the user gave up.
    using PasswordProvider = std::function<std::optional<QString>(const QString& archiveName, bool retry)>;

    explicit ImportTask(QObject* parent = nullptr);
    ~ImportTask() override;

    // `extractRoot`: private folder for the archives of this import (empty:
    // archives are not opened).
    bool start(const QStringList& paths, const std::filesystem::path& extractRoot = {});
    void cancel();
    [[nodiscard]] bool running() const { return running_; }
    void setPasswordProvider(PasswordProvider provider) { passwordProvider_ = std::move(provider); }
    // Valid after finished() was emitted.
    [[nodiscard]] const ScanResult& result() const { return result_; }

Q_SIGNALS:
    void progress(int processed, int total, int imagesFound, const QString& archive);
    void finished();

private:
    ExtractResult expand(const std::filesystem::path& archive, const std::filesystem::path& dest,
                         const std::string& displayPrefix);
    std::optional<std::string> askPassword(const QString& archiveName, bool retry);

    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    ScanResult result_;
    PasswordProvider passwordProvider_;
};

}  // namespace vtc
