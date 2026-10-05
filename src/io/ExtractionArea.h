#pragma once

#include <QLockFile>
#include <QString>
#include <filesystem>
#include <memory>

namespace vtc {

// Private folder where compressed exams are expanded during a session.
//
// Layout: <base>/sessao-<id>/importacao-N/... with <base> in the user's cache
// folder (never a shared /tmp: the files are patient images). The folder is
// readable only by the user, emptied by "Fechar estudos", removed when the
// program exits and, if the program crashed, removed at the next start
// (a lock file tells sessions of running instances from abandoned ones).
class ExtractionArea {
public:
    // `baseDir` empty: <cache location>/extracoes.
    explicit ExtractionArea(const QString& baseDir = QString());
    ~ExtractionArea();
    ExtractionArea(const ExtractionArea&) = delete;
    ExtractionArea& operator=(const ExtractionArea&) = delete;

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] QString sessionDir() const { return session_; }
    // New empty folder for one import (archives of that import go inside).
    std::filesystem::path newImportDir();
    // Deletes everything extracted so far (keeps the session folder).
    void clear();

    // Removes session folders left behind by instances that are no longer
    // running. Returns how many were removed.
    static int removeAbandonedSessions(const QString& baseDir);
    static QString defaultBaseDir();

private:
    QString base_;
    QString session_;
    std::unique_ptr<QLockFile> lock_;
    int counter_ = 0;
    bool valid_ = false;
};

}  // namespace vtc
