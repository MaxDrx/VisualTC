#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

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
    // Blocking decode of every frame of the instance containing `ref`
    // (multi-frame volumes: one decode instead of one per frame).
    std::vector<DecodedFramePtr> decodeAllNow(const FrameRef& ref, QString* error = nullptr);

    void setCacheBudget(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t cacheUsed() const { return cache_.usedBytes(); }
    [[nodiscard]] std::uint64_t cacheBudget() const { return cache_.budget(); }
    [[nodiscard]] int pendingCount() const;
    // Number of file decodes performed so far (diagnostics and tests).
    [[nodiscard]] int decodeCount() const { return decodes_.load(); }
    void clear();
    void shutdown();

Q_SIGNALS:
    // Emitted (in the GUI thread) when all frames of a file are cached or
    // when decoding it failed.
    void instanceReady(const QString& filePath);

private:
    struct Pending {
        int priority = 0;
        int focusFrame = 0;   // frame the most urgent requester wants
        InstancePtr instance; // header the decoded frames must match
    };
    struct Failure {
        std::string message;
        std::int64_t whenMs = 0;
    };

    void runOne();
    // Validates the decoded frames against the header and caches them, the
    // focus frame last so that the LRU keeps the frames nearest to it.
    // Returns the error to report (empty when the focus frame is cached).
    std::string storeResult(const std::string& path, const DecodeResult& result, const InstancePtr& instance,
                            int focusFrame);
    void recordError(const std::string& path, const std::string& message);

    QThreadPool pool_;
    mutable QMutex mutex_;
    std::map<std::string, Pending> pending_;
    std::set<std::string> inFlight_;
    std::map<std::string, Failure> errors_;
    FrameCache cache_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> decodes_{0};
};

}  // namespace vtc
