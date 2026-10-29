#pragma once

#include <QFutureWatcher>
#include <QMainWindow>
#include <atomic>
#include <memory>
#include <vector>

#include "mpr/ImageVolume.h"
#include "render3d/RenderVolume.h"
#include "render3d/Sculpt.h"
#include "render3d/TransferFunction.h"

class QAction;
class QActionGroup;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QStackedWidget;
class QToolButton;

namespace vtc {

class RangeSlider;
class TransferFunctionEditor;
class VolumeRenderView;

// Window of the 3D reconstruction of one series: the view, ready-made
// presets with previews, colour/opacity editing, crop box, 3D scissors,
// standard views, 360° rotation, image export and the choice between the
// graphics card and the processor.
class VolumeRenderWindow : public QMainWindow {
    Q_OBJECT
public:
    VolumeRenderWindow(VolumePtr source, const QString& seriesId, const QString& modality, const QString& title,
                       QWidget* parent = nullptr);
    ~VolumeRenderWindow() override;

    [[nodiscard]] QString seriesId() const { return seriesId_; }
    [[nodiscard]] VolumeRenderView* view() const { return view_; }
    [[nodiscard]] bool isReady() const { return base_ != nullptr; }
    [[nodiscard]] int cutCount() const { return static_cast<int>(cuts_.size()); }
    [[nodiscard]] bool busy() const;
    void applyPreset(const QString& key);
    [[nodiscard]] QString presetKey() const { return presetKey_; }
    // Applies a cut as if drawn with the scissors (tests).
    void addCut(const SculptCut& cut);
    void undoCut();
    void restoreVolume();

Q_SIGNALS:
    void volumeReady();
    void cutsApplied();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    struct UserPreset {
        QString name;
        QString group;  // "ct", "mr", "other"
        RenderPreset preset;
        float opacity = 1.0F;
    };

    void buildUi();
    QWidget* buildPanel();
    void createActions();
    void buildBaseVolume();
    void baseVolumeBuilt();
    void recomputeCuts();
    void cutsFinished();
    void showVolume(const RenderVolumePtr& volume, bool keepCamera);
    void fillPresetList();
    void makeThumbnails();
    void applyResolved(const RenderPreset& preset, float opacity);
    void modeChanged(int index);
    void syncModeWidgets();
    void syncSettingsWidgets();
    void updateStatus();
    void updateCropLabels();
    void saveImage();
    void copyImage();
    void exportRotation();
    void saveUserPreset();
    void deleteUserPreset(int index);
    [[nodiscard]] QString group() const;
    [[nodiscard]] QString presetName(const QString& key) const;
    [[nodiscard]] std::vector<UserPreset> loadUserPresets() const;
    void storeUserPresets(const std::vector<UserPreset>& list) const;
    void pushSettings();

    VolumePtr source_;
    QString seriesId_;
    bool isCt_ = false;
    bool isMr_ = false;
    RenderVolumePtr base_;     // resampled from the series, before cuts
    RenderVolumePtr current_;  // with the cuts
    std::vector<SculptCut> cuts_;
    bool cutsDirty_ = false;
    QString presetKey_;
    std::shared_ptr<std::atomic<bool>> buildCancel_;

    QFutureWatcher<RenderVolume::BuildResult> buildWatcher_;
    QFutureWatcher<RenderVolumePtr> cutsWatcher_;
    QFutureWatcher<std::vector<QImage>> thumbWatcher_;
    std::vector<QString> thumbKeys_;

    VolumeRenderView* view_ = nullptr;
    TransferFunctionEditor* tfEditor_ = nullptr;
    QListWidget* presetList_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QStackedWidget* modeStack_ = nullptr;
    QSlider* opacitySlider_ = nullptr;
    QLabel* opacityValue_ = nullptr;
    QDoubleSpinBox* surfaceSpin_ = nullptr;
    QPushButton* surfaceColorButton_ = nullptr;
    QDoubleSpinBox* windowWidthSpin_ = nullptr;
    QDoubleSpinBox* windowCenterSpin_ = nullptr;
    QCheckBox* shadingCheck_ = nullptr;
    QCheckBox* perspectiveCheck_ = nullptr;
    QComboBox* backgroundCombo_ = nullptr;
    RangeSlider* crop_[3] = {nullptr, nullptr, nullptr};
    QLabel* cropLo_[3] = {nullptr, nullptr, nullptr};
    QLabel* cropHi_[3] = {nullptr, nullptr, nullptr};
    QPushButton* removeInsideButton_ = nullptr;
    QPushButton* removeOutsideButton_ = nullptr;
    QPushButton* undoCutButton_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QComboBox* backendCombo_ = nullptr;
    QComboBox* qualityCombo_ = nullptr;
    QLabel* rendererLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QActionGroup* toolGroup_ = nullptr;
    QAction* actRotate_ = nullptr;
    QAction* actPan_ = nullptr;
    QAction* actZoom_ = nullptr;
    QAction* actWindow_ = nullptr;
    QAction* actScissors_ = nullptr;
    QAction* actSpin_ = nullptr;
    QAction* actResetView_ = nullptr;
    QAction* actSave_ = nullptr;
    QAction* actCopy_ = nullptr;
    QAction* actExport360_ = nullptr;
    bool updating_ = false;
};

}  // namespace vtc
