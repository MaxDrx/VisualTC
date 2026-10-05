#pragma once

#include <map>
#include <vector>

#include "dicom/DicomStudy.h"
#include "viewer2d/ImageSource.h"

namespace vtc {

class FrameProvider;

// Native images of one series, in spatial display order.
class StackSource : public ImageSource {
    Q_OBJECT
public:
    StackSource(SeriesPtr series, FrameProvider* provider, bool preload, QObject* parent = nullptr);
    ~StackSource() override;

    [[nodiscard]] Kind kind() const override { return Kind::Stack; }
    [[nodiscard]] int count() const override { return series_->frameCount(); }
    DecodedFramePtr image(int index) override;
    [[nodiscard]] QString errorAt(int index) const override;
    [[nodiscard]] FrameGeometry geometryAt(int index) const override;
    [[nodiscard]] QSize sizeAt(int index) const override;
    [[nodiscard]] std::string frameOfReference() const override;
    [[nodiscard]] const InstanceInfo* instanceAt(int index) const override;
    [[nodiscard]] const FrameInfo* frameInfoAt(int index) const override;
    [[nodiscard]] QString seriesLabel() const override;
    [[nodiscard]] std::string stateKey() const override { return "series:" + series_->id; }
    [[nodiscard]] std::string annotationKey(int index) const override;
    [[nodiscard]] bool isCt() const override;
    [[nodiscard]] double frameRate() const override;
    void prefetchAround(int index) override;
    void stopPrefetch() override;

    [[nodiscard]] const SeriesPtr& series() const { return series_; }

private:
    void onInstanceReady(const QString& path);
    [[nodiscard]] const FrameRef& ref(int index) const;

    SeriesPtr series_;
    FrameProvider* provider_;
    bool preload_;
    bool preloadIssued_ = false;
    std::map<std::string, std::vector<int>> indicesByPath_;
};

}  // namespace vtc
