#include "io/FrameProvider.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QRunnable>
#include <algorithm>
#include <numeric>

#include "dicom/DicomDecoder.h"
#include "io/DecoderClient.h"

namespace vtc {

namespace {

// A failed file is not decoded again automatically: only an explicit request
// for the visible image, and not before this delay. Without it, every repaint
// of an unreadable slice would start a new decode (and restart the isolated
// decoder in a tight loop).
constexpr std::int64_t kRetryAfterMs = 10'000;

std::int64_t nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// Insertion order for the LRU cache: farthest from the focus frame first, the
// focus frame last (most recently used). When a multi-frame file does not fit
// in the cache, the frames around the one on screen survive.
std::vector<std::size_t> cacheOrder(std::size_t count, int focus) {
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto f = static_cast<long long>(std::clamp<long long>(focus, 0, static_cast<long long>(count) - 1));
    std::stable_sort(order.begin(), order.end(), [f](std::size_t a, std::size_t b) {
        return std::llabs(static_cast<long long>(a) - f) > std::llabs(static_cast<long long>(b) - f);
    });
    return order;
}

// The decoded frames must describe the image announced by the header that
// was scanned; a file replaced or modified since then is reported instead of
// being shown with the wrong geometry.
std::string validateAgainstHeader(const DecodeResult& result, const InstancePtr& instance) {
    if (result.frames.empty()) {
        return "O arquivo não contém quadros decodificáveis.";
    }
    if (!instance) {
        return {};
    }
    for (const auto& f : result.frames) {
        if (!f || f->width != instance->columns || f->height != instance->rows) {
            return "As dimensões da imagem decodificada não correspondem ao cabeçalho lido na importação (o arquivo "
                   "foi alterado?).";
        }
    }
    return {};
}

}  // namespace

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
        if (const auto e = errors_.find(path); e != errors_.end()) {
            if (priority < Visible || nowMs() - e->second.whenMs < kRetryAfterMs) {
                return;  // known-bad file: no automatic retry loop
            }
        }
        auto it = pending_.find(path);
        if (it != pending_.end()) {
            if (priority > it->second.priority) {
                it->second.priority = priority;
                it->second.focusFrame = ref.frame;  // the most urgent requester decides
            }
            return;  // already queued; a free thread will pick the best item
        }
        pending_[path] = Pending{priority, ref.frame, ref.instance};
    }
    pool_.start([this] { runOne(); });
}

void FrameProvider::cancelPending(int maxPriority) {
    QMutexLocker lock(&mutex_);
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.priority <= maxPriority) {
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
    return it == errors_.end() ? QString() : QString::fromStdString(it->second.message);
}

void FrameProvider::runOne() {
    std::string path;
    Pending job;
    {
        QMutexLocker lock(&mutex_);
        if (stopping_ || pending_.empty()) {
            return;
        }
        auto best = pending_.begin();
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->second.priority > best->second.priority) {
                best = it;
            }
        }
        path = best->first;
        job = best->second;
        pending_.erase(best);
        inFlight_.insert(path);
    }
    const DecodeResult result = DecoderClient::decode(path);
    ++decodes_;
    const std::string error = storeResult(path, result, job.instance, job.focusFrame);
    {
        // Record the failure before the file leaves the in-flight set, so a
        // request arriving in between cannot schedule another decode.
        QMutexLocker lock(&mutex_);
        if (!error.empty()) {
            errors_[path] = Failure{error, nowMs()};
        }
        inFlight_.erase(path);
    }
    if (!stopping_) {
        Q_EMIT instanceReady(QString::fromStdString(path));
    }
}

void FrameProvider::recordError(const std::string& path, const std::string& message) {
    QMutexLocker lock(&mutex_);
    errors_[path] = Failure{message.empty() ? std::string("Falha ao decodificar a imagem.") : message, nowMs()};
}

std::string FrameProvider::storeResult(const std::string& path, const DecodeResult& result,
                                       const InstancePtr& instance, int focusFrame) {
    if (!result.ok()) {
        return result.error.empty() ? std::string("Falha ao decodificar a imagem.") : result.error;
    }
    if (std::string why = validateAgainstHeader(result, instance); !why.empty()) {
        return why;
    }
    for (const std::size_t i : cacheOrder(result.frames.size(), focusFrame)) {
        cache_.put(path + "#" + std::to_string(i), result.frames[i]);
    }
    if (focusFrame < 0 || static_cast<std::size_t>(focusFrame) >= result.frames.size()) {
        return "O quadro " + std::to_string(focusFrame + 1) + " não existe no arquivo (" +
               std::to_string(result.frames.size()) + " quadros decodificados).";
    }
    QMutexLocker lock(&mutex_);
    errors_.erase(path);
    return {};
}

DecodedFramePtr FrameProvider::decodeNow(const FrameRef& ref, QString* error) {
    if (auto f = cache_.get(ref.key())) {
        return f;
    }
    const std::string& path = ref.instance->filePath;
    const DecodeResult result = DecoderClient::decode(path);
    ++decodes_;
    const std::string why = storeResult(path, result, ref.instance, ref.frame);
    if (!why.empty()) {
        if (error != nullptr) {
            *error = QString::fromStdString(why);
        }
        recordError(path, why);
        return nullptr;
    }
    return result.frames[static_cast<std::size_t>(ref.frame)];
}

std::vector<DecodedFramePtr> FrameProvider::decodeAllNow(const FrameRef& ref, QString* error) {
    const std::string& path = ref.instance->filePath;
    const DecodeResult result = DecoderClient::decode(path);
    ++decodes_;
    const std::string why = storeResult(path, result, ref.instance, ref.frame);
    if (!why.empty()) {
        if (error != nullptr) {
            *error = QString::fromStdString(why);
        }
        recordError(path, why);
        return {};
    }
    return result.frames;
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
