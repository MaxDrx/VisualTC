#include "io/ThumbnailProvider.h"

#include <QPainter>

#include "imaging/WindowLevel.h"
#include "io/FrameProvider.h"

namespace vtc {

QImage renderFrameToImage(const DecodedFrame& frame, double center, double width, bool invert) {
    DisplayRenderer renderer;
    DisplayParams params;
    params.center = center;
    params.width = width;
    params.invert = invert;
    if (frame.isColor()) {
        QImage img(frame.width, frame.height, QImage::Format_RGB32);
        renderer.renderRgb32(frame, params, reinterpret_cast<std::uint32_t*>(img.bits()),
                             static_cast<int>(img.bytesPerLine()));
        return img;
    }
    QImage img(frame.width, frame.height, QImage::Format_Grayscale8);
    renderer.renderGray(frame, params, img.bits(), static_cast<int>(img.bytesPerLine()));
    return img;
}

WindowPreset defaultWindowFor(const FrameInfo& info, const DecodedFrame& frame) {
    if (frame.isColor()) {
        return {"RGB", 127.5, 256.0};
    }
    if (!info.windows.empty()) {
        const auto& w = info.windows.front();
        return {w.explanation.empty() ? std::string("DICOM") : w.explanation, w.center, w.width};
    }
    return autoWindow(frame);
}

ThumbnailProvider::ThumbnailProvider(FrameProvider* provider, int size, QObject* parent)
    : QObject(parent), provider_(provider), size_(size) {
    connect(provider_, &FrameProvider::instanceReady, this, &ThumbnailProvider::onInstanceReady);
}

void ThumbnailProvider::request(const SeriesPtr& series) {
    if (!series || series->frames.empty() || thumbs_.count(series->id) != 0) {
        return;
    }
    const FrameRef ref = series->frames[series->frames.size() / 2];
    if (provider_->cached(ref)) {
        build(series, ref);
        return;
    }
    waiting_[ref.instance->filePath].push_back({series, ref});
    provider_->request(ref, FrameProvider::Thumbnail);
}

QImage ThumbnailProvider::thumbnail(const std::string& seriesId) const {
    const auto it = thumbs_.find(seriesId);
    return it == thumbs_.end() ? QImage() : it->second;
}

void ThumbnailProvider::clear() {
    waiting_.clear();
    thumbs_.clear();
}

void ThumbnailProvider::onInstanceReady(const QString& path) {
    const auto it = waiting_.find(path.toStdString());
    if (it == waiting_.end()) {
        return;
    }
    const auto items = std::move(it->second);
    waiting_.erase(it);
    for (const auto& [series, ref] : items) {
        build(series, ref);
    }
}

void ThumbnailProvider::build(const SeriesPtr& series, const FrameRef& ref) {
    QImage square(size_, size_, QImage::Format_RGB32);
    square.fill(Qt::black);
    if (const auto frame = provider_->cached(ref)) {
        const auto win = defaultWindowFor(ref.info(), *frame);
        const QImage img = renderFrameToImage(*frame, win.center, win.width, false);
        // Respect the physical aspect ratio (non-square pixels).
        const auto& g = ref.geometry();
        const double physW = frame->width * (g.hasSpacing() ? g.spacingX : 1.0);
        const double physH = frame->height * (g.hasSpacing() ? g.spacingY : 1.0);
        const double scale = std::min(size_ / physW, size_ / physH);
        const QSizeF target(physW * scale, physH * scale);
        QPainter p(&square);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(QPointF((size_ - target.width()) / 2.0, (size_ - target.height()) / 2.0), target), img);
    }
    thumbs_[series->id] = square;
    Q_EMIT thumbnailReady(QString::fromStdString(series->id));
}

}  // namespace vtc
