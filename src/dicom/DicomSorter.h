#pragma once

#include <string>
#include <vector>

#include "dicom/DicomSeries.h"

namespace vtc {

struct StackSplit {
    std::string label;             // sub label (pt-BR), empty when not split
    std::vector<FrameRef> frames;  // display order
};

// Builds the ordered stacks of one SeriesInstanceUID.
//
// Ordering never relies on InstanceNumber alone: frames of a spatial stack
// are sorted by their position along the slice normal (ImagePositionPatient
// projected on ImageOrientationPatient row x column). InstanceNumber is only
// used to choose the scroll direction (so slice 1 matches the scanner) and to
// order non-spatial images (localizers, cine, projection radiography).
std::vector<StackSplit> buildStacks(std::vector<FrameRef> frames);

// Sorts frames spatially along `normal`; ties broken by instance/frame number.
void sortSpatially(std::vector<FrameRef>& frames);

// Sorts by InstanceNumber, then frame index, then content time, then path.
void sortByInstance(std::vector<FrameRef>& frames);

}  // namespace vtc
