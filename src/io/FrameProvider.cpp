#include "io/FrameProvider.h"

#include <QMutexLocker>
#include <QRunnable>

#include "dicom/DicomDecoder.h"
#include "io/DecoderClient.h"

namespace vtc {

FrameProvider::FrameProvider(std::uint64_t cacheBytes, int threads, QObject* parent)
    : QObject(parent), cache_(cacheBytes) {
    pool_.setMaxThreadCount(std::max(1, threads));
    // Keep threads (and their isolated decoder processes) alive while the
    // user is reading: restarting a worker costs a few milliseconds.
    pool_.setExpiryTimeout(5 * 60 * 1000);
}

FrameProvider::~FrameProvider() { shutdown(); }

void FrameProvider::shutdown() {
    stopping_ = true;
    {
        QMutexLocker lock(&mutex_);
        pending_.clear();
    }
    pool_.waitForDone();
}

DecodedFramePtr FrameProvider::cached(const FrameRef& ref) { return cache_.get(ref.key()); }

void FrameProvider::request(const FrameRef& ref, int priority) {
    if (stopping_ || !ref.instance) {
        return;
    }
    if (cache_.contains(ref.key())) {
        return;
    }
    const std::string& path = ref.instance->filePath;
    {
        QMutexLocker lock(&mutex_);
        if (inFlight_.count(path) != 0) {
            return;
        }
        if (errors_.count(path) != 0 && priority < Visible) {
            return;  // known-bad file: retry only when explicitly displayed
        }
        auto it = pending_.find(path);
        if (it != pending_.end()) {
            it->second = std::max(it->second, priority);
            return;  // already queued; a free thread will pick the best item
        }
        pending_[path] = priority;
    }
    pool_.start([this] { runOne(); });
}

void FrameProvider::cancelPending(int maxPriority) {
    QMutexLocker lock(&mutex_);
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second <= maxPriority) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

int FrameProvider::pendingCount() const {
    QMutexLocker lock(&mutex_);
    return static_cast<int>(pending_.size() + inFlight_.size());
}

QString FrameProvider::errorFor(const std::string& filePath) const {
    QMutexLocker lock(&mutex_);
    const auto it = errors_.find(filePath);
    return it == errors_.end() ? QString() : QString::fromStdString(it->second);
}

void FrameProvider::runOne() {
    std::string path;
    {
        QMutexLocker lock(&mutex_);
        if (stopping_ || pending_.empty()) {
            return;
        }
        auto best = pending_.begin();
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->second > best->second) {
                best = it;
            }
        }
        path = best->first;
        pending_.erase(best);
        inFlight_.insert(path);
    }
    const DecodeResult result = DecoderClient::decode(path);
    storeResult(path, result);
    if (!stopping_) {
        Q_EMIT instanceReady(QString::fromStdString(path));
    }
}

void FrameProvider::storeResult(const std::string& path, const DecodeResult& result) {
    if (result.ok()) {
        for (std::size_t i = 0; i < result.frames.size(); ++i) {
            cache_.put(path + "#" + std::to_string(i), result.frames[i]);
        }
    }
    QMutexLocker lock(&mutex_);
    inFlight_.erase(path);
    if (result.ok()) {
        errors_.erase(path);
    } else {
        errors_[path] = result.error.empty() ? std::string("Falha ao decodificar a imagem.") : result.error;
    }
}

DecodedFramePtr FrameProvider::decodeNow(const FrameRef& ref, QString* error) {
    if (auto f = cache_.get(ref.key())) {
        return f;
    }
    const std::string& path = ref.instance->filePath;
    const DecodeResult result = DecoderClient::decode(path);
    if (!result.ok()) {
        if (error != nullptr) {
            *error = QString::fromStdString(result.error);
        }
        QMutexLocker lock(&mutex_);
        errors_[path] = result.error;
        return nullptr;
    }
    for (std::size_t i = 0; i < result.frames.size(); ++i) {
        cache_.put(path + "#" + std::to_string(i), result.frames[i]);
    }
    const auto idx = static_cast<std::size_t>(ref.frame);
    return idx < result.frames.size() ? result.frames[idx] : nullptr;
}

void FrameProvider::setCacheBudget(std::uint64_t bytes) { cache_.setBudget(bytes); }

void FrameProvider::clear() {
    {
        QMutexLocker lock(&mutex_);
        pending_.clear();
        errors_.clear();
    }
    cache_.clear();
}

}  // namespace vtc
