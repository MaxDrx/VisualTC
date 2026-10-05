#include "io/ExtractionArea.h"

#include <QDir>
#include <QStandardPaths>
#include <QUuid>
#include <system_error>

#include "core/Logger.h"
#include "core/PathUtil.h"

namespace vtc {

namespace {

void makePrivate(const QString& dir) {
    std::error_code ec;
    std::filesystem::permissions(utf8ToPath(dir.toStdString()), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
}

void removeTree(const QString& dir) {
    std::error_code ec;
    std::filesystem::remove_all(utf8ToPath(dir.toStdString()), ec);
}

}  // namespace

QString ExtractionArea::defaultBaseDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/extracoes");
}

ExtractionArea::ExtractionArea(const QString& baseDir) : base_(baseDir.isEmpty() ? defaultBaseDir() : baseDir) {
    if (!QDir().mkpath(base_)) {
        logWarning("archive", "Não foi possível criar a pasta de extração.");
        return;
    }
    makePrivate(base_);
    removeAbandonedSessions(base_);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    session_ = base_ + QStringLiteral("/sessao-") + id;
    lock_ = std::make_unique<QLockFile>(session_ + QStringLiteral(".lock"));
    lock_->setStaleLockTime(0);  // stale only when the owning process is gone
    if (!lock_->tryLock(0) || !QDir().mkpath(session_)) {
        logWarning("archive", "Não foi possível preparar a pasta de extração da sessão.");
        lock_.reset();
        return;
    }
    makePrivate(session_);
    valid_ = true;
}

ExtractionArea::~ExtractionArea() {
    if (valid_) {
        removeTree(session_);
    }
    if (lock_) {
        lock_->unlock();
    }
}

std::filesystem::path ExtractionArea::newImportDir() {
    const QString dir = session_ + QStringLiteral("/importacao-") + QString::number(++counter_);
    QDir().mkpath(dir);
    makePrivate(dir);
    return utf8ToPath(dir.toStdString());
}

void ExtractionArea::clear() {
    if (!valid_) {
        return;
    }
    const QDir d(session_);
    for (const QString& entry : d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        removeTree(d.filePath(entry));
    }
}

int ExtractionArea::removeAbandonedSessions(const QString& baseDir) {
    int removed = 0;
    const QDir base(baseDir);
    for (const QString& name : base.entryList({QStringLiteral("sessao-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        QLockFile lock(base.filePath(name + QStringLiteral(".lock")));
        lock.setStaleLockTime(0);
        // tryLock succeeds when nobody holds the lock or when its owner
        // process no longer exists (Qt removes such stale lock files).
        if (lock.tryLock(0)) {
            removeTree(base.filePath(name));
            lock.unlock();
            ++removed;
        }
    }
    if (removed > 0) {
        logInfo("archive", "Removidas " + std::to_string(removed) + " pastas de extração de sessões anteriores.");
    }
    return removed;
}

}  // namespace vtc
