#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QSpinBox;
class QTableWidget;

namespace vtc {

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget* parent = nullptr);
    void accept() override;

private:
    QCheckBox* darkTheme_;
    QSpinBox* fontSize_;
    QComboBox* accent_;
    QComboBox* language_;
    QComboBox* left_;
    QComboBox* middle_;
    QComboBox* right_;
    QSpinBox* cacheMb_;
    QCheckBox* preload_;
    QSpinBox* threads_;
    QCheckBox* smooth_;
    QCheckBox* isolated_;
    QComboBox* quality_;
    QCheckBox* overlays_;
    QCheckBox* referenceLines_;
    QCheckBox* syncZoomPan_;
    QCheckBox* syncWindow_;
    QTableWidget* presets_;
};

}  // namespace vtc
