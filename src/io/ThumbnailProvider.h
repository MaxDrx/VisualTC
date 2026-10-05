#pragma once

#include <QImage>
#include <QObject>
#include <map>
#include <string>
#include <vector>

#include "dicom/DicomSeries.h"
#include "imaging/PixelData.h"
#include "imaging/WindowLevel.h"

namespace vtc {

class FrameProvider;

// Renders a DICOM frame to a QImage with a given window (grey or RGB).
QImage renderFrameToImage(const DecodedFrame& frame, double center, double width, bool invert);

// Default window for a frame: first DICOM window of the file, else a robust
// window from the data. Never a hard-coded value (section 14).
WindowPreset defaultWindowFor(const FrameInfo& info, const DecodedFrame& frame);

// Series thumbnails generated in the background (middle image of each
// series). Opening a study never waits for thumbnails.
class ThumbnailProvider : public QObject {
    Q_OBJECT
public:
    ThumbnailProvider(FrameProvider* provider, int size, QObject* parent = nullptr);

    void request(const SeriesPtr& series);
    [[nodiscard]] QImage thumbnail(const std::string& seriesId) const;
    void clear();

Q_SIGNALS:
    void thumbnailReady(const QString& seriesId);

private:
    void onInstanceReady(const QString& path);
    void build(const SeriesPtr& series, const FrameRef& ref);

    FrameProvider* provider_;
    int size_;
    std::map<std::string, std::vector<std::pair<SeriesPtr, FrameRef>>> waiting_;
    std::map<std::string, QImage> thumbs_;
};

}  // namespace vtc
