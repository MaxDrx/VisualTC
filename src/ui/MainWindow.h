#pragma once

#include <QMainWindow>
#include <QStringList>
#include <atomic>
#include <map>
#include <memory>
#include <thread>

#include "dicom/DicomScanner.h"
#include "dicom/DicomStudy.h"
#include "imaging/WindowLevel.h"
#include "viewer2d/Viewport.h"

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QMenu;
class QProgressBar;
class QTimer;
class QToolButton;

namespace vtc {

class AnnotationStore;
class ExtractionArea;
class FrameProvider;
class ImportTask;
class SeriesBrowser;
class SeriesDock;
class ThumbnailProvider;
class ViewerGrid;
class MprSession;

// Options used for automated smoke tests and documentation screenshots
// (e.g. `VisualTC --screenshot out.png --layout 2x2 pasta/`).
struct AutomationOptions {
    QString screenshot;
    int rows = 0;
    int cols = 0;
    bool mpr = false;
    bool sync = false;
    QString demoMeasurements;  // "1" to draw demo measurements
    int slab = 0;
    QString preset;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    void importPaths(const QStringList& paths);
    void setAutomation(const AutomationOptions& options);

protected:
    // Documents handed over by the system (macOS QFileOpenEvent: double-click
    // on a .dcm, "Abrir com", a CD folder dropped on the Dock icon).
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void createActions();
    void createMenus();
    void createToolbar();
    void createDock();
    void createStatusBar();

    void openFiles();
    void openFolder();
    void openArchive();
    void closeStudies();
    void onImportProgress(int processed, int total, int found, const QString& archive);
    void onImportFinished();
    void showImportIssues();
    void updateEmptyHint();
    void openSeries(const QString& seriesId, Viewport* target = nullptr);
    void startMpr(const QString& seriesId = QString());
    void toggleMpr(bool on);
    void setTool(Tool tool);
    void applyPreset(const WindowPreset& preset);
    void rebuildPresetMenu();
    void saveCurrentWindowAsPreset();
    void onActiveChanged();
    void updateActionStates();
    void updateStatusInfo();
    void showRoiHistogram();
    void showDicomInfo(const QString& seriesId = QString());
    void showPreferences();
    void showAbout();
    void showShortcuts();
    void setSlab(double thickness, int mode);
    void runAutomationStep();
    QString activeSeriesId() const;

    StudyDatabase db_;
    AnnotationStore* annotations_ = nullptr;
    FrameProvider* frames_ = nullptr;
    ThumbnailProvider* thumbs_ = nullptr;
    ImportTask* import_ = nullptr;
    ViewerGrid* grid_ = nullptr;
    SeriesBrowser* browser_ = nullptr;
    SeriesDock* browserDock_ = nullptr;

    QLabel* cursorLabel_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QLabel* cacheLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QToolButton* cancelButton_ = nullptr;

    std::map<Tool, QAction*> toolActions_;
    QActionGroup* toolGroup_ = nullptr;
    QAction* actOpenFiles_ = nullptr;
    QAction* actOpenFolder_ = nullptr;
    QAction* actOpenArchive_ = nullptr;
    QAction* actStudies_ = nullptr;
    QAction* actExport_ = nullptr;
    QAction* actCapture_ = nullptr;
    QAction* actSync_ = nullptr;
    QAction* actRefLines_ = nullptr;
    QAction* actOverlays_ = nullptr;
    QAction* actInvert_ = nullptr;
    QAction* actRotCw_ = nullptr;
    QAction* actRotCcw_ = nullptr;
    QAction* actFlipH_ = nullptr;
    QAction* actFlipV_ = nullptr;
    QAction* actFit_ = nullptr;
    QAction* actActual_ = nullptr;
    QAction* actReset_ = nullptr;
    QAction* actMpr_ = nullptr;
    QAction* act3d_ = nullptr;
    QAction* actCine_ = nullptr;
    QAction* actUndo_ = nullptr;
    QAction* actRedo_ = nullptr;
    QAction* actDelete_ = nullptr;
    QAction* actDeleteAll_ = nullptr;
    QAction* actHistogram_ = nullptr;
    QAction* actInfo_ = nullptr;
    QAction* actVoiLut_ = nullptr;
    QMenu* presetMenu_ = nullptr;
    QMenu* layoutMenu_ = nullptr;
    QMenu* measureMenu_ = nullptr;
    QMenu* roiMenu_ = nullptr;
    QMenu* slabMenu_ = nullptr;

    // MPR volume construction
    std::thread mprThread_;
    std::atomic<bool> mprCancel_{false};
    bool mprBuilding_ = false;

    // Import bookkeeping: problems of every import of this session, and a
    // flag to drop the result of an import interrupted by "Fechar estudos".
    std::vector<ScanIssue> importIssues_;
    // Extracted copies of archive members -> "exame.zip › pasta/arquivo".
    std::map<std::string, std::string> displayNames_;
    std::unique_ptr<ExtractionArea> extraction_;
    QTimer* fileOpenTimer_ = nullptr;
    QStringList fileOpenQueue_;
    bool discardImport_ = false;
    QAction* issuesAction_ = nullptr;

    AutomationOptions automation_;
    int automationStage_ = 0;
};

}  // namespace vtc
