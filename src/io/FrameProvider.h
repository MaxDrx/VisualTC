#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <map>
#include <set>
#include <string>

#include "dicom/DicomDecoder.h"
#include "dicom/DicomTypes.h"
#include "imaging/FrameCache.h"

namespace vtc {

// Asynchronous, prioritized frame loading with an LRU cache.
//
// Requests are kept in a priority map rather than a FIFO: when a decode
// thread becomes free it takes the most urgent pending file. Re-requesting a
// file with a higher priority (e.g. the slice the user just scrolled to)
// moves it to the front, so fast scrolling never waits behind stale
// prefetch work. The UI thread never decodes.
class FrameProvider : public QObject {
    Q_OBJECT
public:
    enum Priority { Background = 0, Thumbnail = 2, Prefetch = 5, Visible = 10 };

    FrameProvider(std::uint64_t cacheBytes, int threads, QObject* parent = nullptr);
    ~FrameProvider() override;

    // Non-blocking cache lookup.
    DecodedFramePtr cached(const FrameRef& ref);
    // Schedules decoding of the instance containing `ref`.
    void request(const FrameRef& ref, int priority);
    // Drops queued requests with priority <= maxPriority (in-flight work finishes).
    void cancelPending(int maxPriority);
    // Last decode error for a file (pt-BR), empty if none.
    QString errorFor(const std::string& filePath) const;
    // Blocking decode for worker threads (volume building). Uses the cache.
    DecodedFramePtr decodeNow(const FrameRef& ref, QString* error = nullptr);

    void setCacheBudget(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t cacheUsed() const { return cache_.usedBytes(); }
    [[nodiscard]] std::uint64_t cacheBudget() const { return cache_.budget(); }
    [[nodiscard]] int pendingCount() const;
    void clear();
    void shutdown();

Q_SIGNALS:
    // Emitted (in the GUI thread) when all frames of a file are cached or
    // when decoding it failed.
    void instanceReady(const QString& filePath);

private:
    void runOne();
    void storeResult(const std::string& path, const DecodeResult& result);

    QThreadPool pool_;
    mutable QMutex mutex_;
    std::map<std::string, int> pending_;
    std::set<std::string> inFlight_;
    std::map<std::string, std::string> errors_;
    FrameCache cache_;
    std::atomic<bool> stopping_{false};
};

}  // namespace vtc
