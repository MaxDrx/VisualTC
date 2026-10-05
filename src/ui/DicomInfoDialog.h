#pragma once

#include <QDialog>

#include "dicom/DicomSeries.h"

namespace vtc {

// Read-only view of the main DICOM attributes of an image and of the
// geometric analysis of its series (spacing, gaps, tilt, warnings).
class DicomInfoDialog : public QDialog {
    Q_OBJECT
public:
    DicomInfoDialog(const SeriesPtr& series, int frameIndex, QWidget* parent = nullptr);
};

}  // namespace vtc
