#include "viewer2d/StackSource.h"

#include <algorithm>

#include "app/I18n.h"
#include "io/FrameProvider.h"

namespace vtc {

StackSource::StackSource(SeriesPtr series, FrameProvider* provider, bool preload, QObject* parent)
    : ImageSource(parent), series_(std::move(series)), provider_(provider), preload_(preload) {
    for (int i = 0; i < series_->frameCount(); ++i) {
        indicesByPath_[series_->frames[static_cast<size_t>(i)].instance->filePath].push_back(i);
    }
    connect(provider_, &FrameProvider::instanceReady, this, &StackSource::onInstanceReady);
}

StackSource::~StackSource() = default;

const FrameRef& StackSource::ref(int index) const {
    index = std::clamp(index, 0, std::max(0, count() - 1));
    return series_->frames[static_cast<size_t>(index)];
}

DecodedFramePtr StackSource::image(int index) {
    if (count() == 0) {
        return nullptr;
    }
    const FrameRef& r = ref(index);
    if (auto f = provider_->cached(r)) {
        return f;
    }
    provider_->request(r, FrameProvider::Visible);
    return nullptr;
}

QString StackSource::errorAt(int index) const {
    if (count() == 0) {
        return {};
    }
    return trCore(provider_->errorFor(ref(index).instance->filePath));
}

FrameGeometry StackSource::geometryAt(int index) const { return ref(index).geometry(); }

QSize StackSource::sizeAt(int index) const {
    const auto& inst = *ref(index).instance;
    return {inst.columns, inst.rows};
}

std::string StackSource::frameOfReference() const {
    return series_->frames.empty() ? std::string() : series_->firstInstance().frameOfReferenceUid;
}

const InstanceInfo* StackSource::instanceAt(int index) const {
    return count() == 0 ? nullptr : ref(index).instance.get();
}

const FrameInfo* StackSource::frameInfoAt(int index) const { return count() == 0 ? nullptr : &ref(index).info(); }

QString StackSource::seriesLabel() const {
    QString label;
    if (auto n = series_->number()) {
        label = QString::number(*n) + " · ";
    }
    return label + seriesDescription(*series_);
}

std::string StackSource::annotationKey(int index) const { return ref(index).key(); }

bool StackSource::isCt() const { return series_->modality() == "CT"; }

double StackSource::frameRate() const {
    if (series_->frames.empty()) {
        return 10.0;
    }
    const auto& inst = series_->firstInstance();
    if (inst.recommendedFrameRate && *inst.recommendedFrameRate > 0.0) {
        return std::clamp(*inst.recommendedFrameRate, 1.0, 120.0);
    }
    if (inst.frameTimeMs && *inst.frameTimeMs > 0.0) {
        return std::clamp(1000.0 / *inst.frameTimeMs, 1.0, 120.0);
    }
    return 10.0;
}

void StackSource::prefetchAround(int index) {
    const int n = count();
    // Adjacent slices first (both directions), then optionally the whole
    // series in background, nearest first, as long as it fits the cache.
    for (int d = 1; d <= 6; ++d) {
        for (int s : {index + d, index - d}) {
            if (s >= 0 && s < n) {
                provider_->request(ref(s), FrameProvider::Prefetch);
            }
        }
    }
    if (preload_ && !preloadIssued_) {
        preloadIssued_ = true;
        std::uint64_t estimate = 0;
        for (const auto& f : series_->frames) {
            estimate += static_cast<std::uint64_t>(f.instance->rows) * f.instance->columns *
                        std::max(1, f.instance->bitsAllocated / 8) * f.instance->samplesPerPixel;
        }
        if (estimate < provider_->cacheBudget() * 8 / 10) {
            for (int d = 7; d < n; ++d) {
                for (int s : {index + d, index - d}) {
                    if (s >= 0 && s < n) {
                        provider_->request(ref(s), FrameProvider::Background);
                    }
                }
            }
        }
    }
}

void StackSource::stopPrefetch() { provider_->cancelPending(FrameProvider::Background); }

void StackSource::onInstanceReady(const QString& path) {
    const auto it = indicesByPath_.find(path.toStdString());
    if (it == indicesByPath_.end()) {
        return;
    }
    for (int i : it->second) {
        Q_EMIT imageReady(i);
    }
}

}  // namespace vtc
