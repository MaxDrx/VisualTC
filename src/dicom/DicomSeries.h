#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dicom/DicomGeometry.h"
#include "dicom/DicomTypes.h"

namespace vtc {

// A displayable series: an ordered stack of frames. One SeriesInstanceUID can
// produce several Series when it mixes echoes, phases, orientations or cine
// clips (see DicomSorter).
struct Series {
    std::string id;                 // unique within the database
    std::string seriesInstanceUid;
    std::string subLabel;           // e.g. "Eco 2", "Fase 3", "Clipe 1"; empty if not split
    std::vector<FrameRef> frames;   // display order
    StackGeometry geometry;

    [[nodiscard]] const InstanceInfo& firstInstance() const { return *frames.front().instance; }
    [[nodiscard]] std::string modality() const { return frames.empty() ? std::string() : firstInstance().modality; }
    [[nodiscard]] std::string description() const;  // description + sub label
    [[nodiscard]] std::optional<int> number() const {
        return frames.empty() ? std::nullopt : firstInstance().seriesNumber;
    }
    [[nodiscard]] int frameCount() const { return static_cast<int>(frames.size()); }
    [[nodiscard]] bool isMultiFrameClip() const;
    // Slice thickness of the middle frame, if present.
    [[nodiscard]] std::optional<double> sliceThickness() const;
};

using SeriesPtr = std::shared_ptr<Series>;

}  // namespace vtc
