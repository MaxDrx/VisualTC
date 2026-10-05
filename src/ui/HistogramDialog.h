#pragma once

#include <QDialog>
#include <vector>

#include "measurements/RoiStatistics.h"

namespace vtc {

// Histogram of the modality values inside an ROI (HU x pixel count for CT).
class HistogramDialog : public QDialog {
    Q_OBJECT
public:
    HistogramDialog(const std::vector<double>& values, const QString& unit, QWidget* parent = nullptr);
};

}  // namespace vtc
