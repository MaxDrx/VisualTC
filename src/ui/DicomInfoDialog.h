#pragma once

#include <QDialog>
#include <functional>
#include <string>

#include "dicom/DicomSeries.h"

namespace vtc {

// Read-only view of the main DICOM attributes of an image and of the
// geometric analysis of its series (spacing, gaps, tilt, warnings).
class DicomInfoDialog : public QDialog {
    Q_OBJECT
public:
    // `displayPath` maps a file path to what the user knows (e.g. the member
    // name inside a compressed exam); identity when empty.
    DicomInfoDialog(const SeriesPtr& series, int frameIndex, QWidget* parent = nullptr,
                    const std::function<std::string(const std::string&)>& displayPath = {});
};

}  // namespace vtc
