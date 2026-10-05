#include "ui/MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include "app/AppSettings.h"
#include "app/Theme.h"
#include "core/Logger.h"
#include "core/SystemInfo.h"
#include "dicom/DicomParser.h"
#include "export/ImageExporter.h"
#include "io/DecoderClient.h"
#include "io/ExtractionArea.h"
#include "io/FrameProvider.h"
#include "io/ImportTask.h"
#include "io/ThumbnailProvider.h"
#include "mpr/MprSession.h"
#include "mpr/MprSource.h"
#include "ui/DicomInfoDialog.h"
#include "ui/HistogramDialog.h"
#include "ui/Icons.h"
#include "ui/PreferencesDialog.h"
#include "ui/SeriesBrowser.h"
#include "ui/ViewerGrid.h"
#include "viewer2d/AnnotationStore.h"
#include "viewer2d/StackSource.h"

namespace vtc {

namespace {
constexpr std::uint64_t MiB = 1024ull * 1024ull;

StackSource* stackOf(Viewport* vp) {
    return vp && vp->source() ? dynamic_cast<StackSource*>(vp->source().get()) : nullptr;
}
MprSource* mprOf(Viewport* vp) { return vp && vp->source() ? dynamic_cast<MprSource*>(vp->source().get()) : nullptr; }
}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("VisualTC — DICOM Medical Image Viewer"));
    setWindowIcon(Icons::appIcon());
    setAcceptDrops(true);
    setDockOptions(QMainWindow::AnimatedDocks);

    auto& s = AppSettings::instance();
    const std::uint64_t cache =
        s.cacheMegabytes() > 0 ? static_cast<std::uint64_t>(s.cacheMegabytes()) * MiB : defaultCacheBudgetBytes();
    const int threads = s.decodeThreads() > 0 ? s.decodeThreads()
                                              : std::clamp(static_cast<int>(hardwareThreads()) - 1, 2, 6);
    annotations_ = new AnnotationStore(this);
    frames_ = new FrameProvider(cache, threads, this);
    thumbs_ = new ThumbnailProvider(frames_, 96, this);
    import_ = new ImportTask(this);
    extraction_ = std::make_unique<ExtractionArea>();
    import_->setPasswordProvider([this](const QString& archive, bool retry) -> std::optional<QString> {
        bool ok = false;
        const QString text =
            retry ? tr("Senha incorreta. Digite novamente a senha de %1:").arg(archive)
                  : tr("O arquivo compactado %1 está protegido por senha.\nDigite a senha para abrir o exame:")
                        .arg(archive);
        const QString pw = QInputDialog::getText(this, tr("Arquivo protegido por senha"), text, QLineEdit::Password,
                                                 QString(), &ok);
        if (!ok) {
            return std::nullopt;
        }
        return pw;
    });
    grid_ = new ViewerGrid(annotations_, this);
    setCentralWidget(grid_);

    createActions();
    createMenus();
    createToolbar();
    createDock();
    createStatusBar();

    connect(import_, &ImportTask::progress, this, &MainWindow::onImportProgress);
    connect(import_, &ImportTask::finished, this, &MainWindow::onImportFinished);
    connect(thumbs_, &ThumbnailProvider::thumbnailReady, this,
            [this](const QString& id) { browser_->setThumbnail(id, thumbs_->thumbnail(id.toStdString())); });
    connect(grid_, &ViewerGrid::activeViewportChanged, this, &MainWindow::onActiveChanged);
    connect(grid_, &ViewerGrid::activeStateChanged, this, [this] { updateStatusInfo(); });
    connect(grid_, &ViewerGrid::cursorInfo, cursorLabel_, &QLabel::setText);
    connect(grid_, &ViewerGrid::seriesDropped, this,
            [this](Viewport* vp, const QString& id) { openSeries(id, vp); });
    connect(grid_, &ViewerGrid::displayedSeriesChanged, this, [this] {
        QSet<QString> ids;
        for (auto* vp : grid_->visibleViewports()) {
            if (auto* st = stackOf(vp)) {
                ids.insert(QString::fromStdString(st->series()->id));
            }
        }
        browser_->markDisplayed(ids);
        updateActionStates();
    });
    connect(annotations_->undoStack(), &QUndoStack::canUndoChanged, actUndo_, &QAction::setEnabled);
    connect(annotations_->undoStack(), &QUndoStack::canRedoChanged, actRedo_, &QAction::setEnabled);
    connect(annotations_, &AnnotationStore::changed, this, &MainWindow::updateActionStates);

    auto* cacheTimer = new QTimer(this);
    connect(cacheTimer, &QTimer::timeout, this, [this] {
        cacheLabel_->setText(tr("Cache %1 / %2 MB")
                                 .arg(frames_->cacheUsed() / MiB)
                                 .arg(frames_->cacheBudget() / MiB));
    });
    cacheTimer->start(1000);

    if (!restoreGeometry(s.windowGeometry())) {
        resize(1400, 880);
    }
    restoreState(s.windowState());
    updateEmptyHint();
    setTool(Tool::WindowLevel);
    updateActionStates();
    logInfo("app", "Janela principal criada.");
}

MainWindow::~MainWindow() {
    mprCancel_ = true;
    if (mprThread_.joinable()) {
        mprThread_.join();
    }
    import_->cancel();
    // Viewports hold sources that reference the frame provider: destroy the
    // viewer before the providers (Qt deletes children in creation order).
    delete grid_;
    grid_ = nullptr;
    frames_->shutdown();
    extraction_.reset();  // deletes the images extracted from compressed exams
}

// ------------------------------------------------------------------ actions

void MainWindow::createActions() {
    auto make = [this](const QString& text, const QString& icon, const QKeySequence& key, auto slot,
                       const QString& tip = QString()) {
        auto* a = new QAction(Icons::get(icon), text, this);
        if (!key.isEmpty()) {
            a->setShortcut(key);
        }
        a->setToolTip(tip.isEmpty() ? (key.isEmpty() ? text : text + "  (" + key.toString(QKeySequence::NativeText) + ")")
                                    : tip);
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };

    actOpenFiles_ = make(tr("Abrir arquivos…"), "open-file", QKeySequence::Open, [this] { openFiles(); },
                         tr("Abrir arquivos DICOM ou exames compactados (ZIP, RAR, 7z, TAR, GZ, ISO)  (Ctrl+O)"));
    actOpenArchive_ = make(tr("Abrir exame compactado (ZIP, RAR, 7z)…"), "open-file", QKeySequence(),
                           [this] { openArchive(); });
    actOpenFolder_ = make(tr("Abrir pasta…"), "open-folder", QKeySequence(tr("Ctrl+Shift+O")), [this] { openFolder(); });
    actStudies_ = make(tr("Estudos"), "info", QKeySequence(tr("F2")), [this] {
        browserDock_->setVisible(!browserDock_->isVisible());
    });
    actExport_ = make(tr("Exportar imagem…"), "export", QKeySequence(tr("Ctrl+E")), [this] {
        ImageExporter::exportViewport(this, grid_->activeViewport());
    });
    actCapture_ = make(tr("Capturar viewport"), "capture", QKeySequence(tr("Ctrl+Shift+C")), [this] {
        ImageExporter::captureToClipboard(grid_->activeViewport(), true);
        statusBar()->showMessage(tr("Viewport copiado para a área de transferência."), 4000);
    });

    toolGroup_ = new QActionGroup(this);
    toolGroup_->setExclusive(true);
    struct ToolDef {
        Tool tool;
        QString text;
        QString icon;
        QString key;
    };
    const std::vector<ToolDef> tools = {
        {Tool::WindowLevel, tr("Window/Level"), "window-level", "W"},
        {Tool::Pan, tr("Pan"), "pan", "P"},
        {Tool::Zoom, tr("Zoom"), "zoom", "Z"},
        {Tool::Scroll, tr("Navegar cortes"), "scroll", "S"},
        {Tool::Distance, tr("Régua"), "ruler", "M"},
        {Tool::Angle, tr("Ângulo"), "angle", "A"},
        {Tool::Cobb, tr("Ângulo de Cobb"), "cobb", "C"},
        {Tool::RectRoi, tr("ROI retangular"), "roi-rect", "R"},
        {Tool::EllipseRoi, tr("ROI elíptica"), "roi-ellipse", "E"},
        {Tool::FreehandRoi, tr("ROI livre"), "roi-freehand", "L"},
        {Tool::Probe, tr("Valor do pixel"), "probe", "V"},
        {Tool::Crosshair, tr("Crosshair (MPR)"), "crosshair", "X"},
    };
    for (const auto& t : tools) {
        auto* a = make(t.text, t.icon, QKeySequence(t.key), [this, tool = t.tool] { setTool(tool); });
        a->setCheckable(true);
        toolGroup_->addAction(a);
        toolActions_[t.tool] = a;
    }
    auto* esc = new QAction(this);
    esc->setShortcut(Qt::Key_Escape);
    connect(esc, &QAction::triggered, this, [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->cancelPendingAnnotation();
        }
        setTool(grid_->mprSession() ? Tool::Crosshair : Tool::WindowLevel);
    });
    addAction(esc);

    actInvert_ = make(tr("Inverter"), "invert", QKeySequence(tr("I")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->setInvert(!vp->inverted());
        }
    });
    actRotCw_ = make(tr("Girar 90° horário"), "rotate-cw", QKeySequence(tr("]")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->rotate90(1);
        }
    });
    actRotCcw_ = make(tr("Girar 90° anti-horário"), "rotate-ccw", QKeySequence(tr("[")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->rotate90(-1);
        }
    });
    actFlipH_ = make(tr("Espelhar horizontal"), "flip-h", QKeySequence(tr("H")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->flipHorizontal();
        }
    });
    actFlipV_ = make(tr("Espelhar vertical"), "flip-v", QKeySequence(tr("Shift+H")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->flipVertical();
        }
    });
    actFit_ = make(tr("Ajustar à janela"), "fit", QKeySequence(tr("F")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->fitToWindow();
        }
    });
    actActual_ = make(tr("Tamanho real (1:1)"), "zoom", QKeySequence(tr("Shift+F")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->actualSize();
        }
    });
    actReset_ = make(tr("Reset"), "reset", QKeySequence(tr("Ctrl+0")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->resetView();
        }
    });
    actSync_ = make(tr("Sincronizar séries"), "sync", QKeySequence(tr("Y")), [this] {
        grid_->setSyncEnabled(actSync_->isChecked());
        if (actSync_->isChecked()) {
            const QString problem = grid_->syncProblem();
            statusBar()->showMessage(problem.isEmpty() ? tr("Sincronização espacial ativada.") : problem, 8000);
        }
    });
    actSync_->setCheckable(true);
    actRefLines_ = make(tr("Linhas de referência"), "reference-lines", QKeySequence(tr("Ctrl+L")), [this] {
        grid_->setReferenceLinesEnabled(actRefLines_->isChecked());
        AppSettings::instance().setReferenceLines(actRefLines_->isChecked());
    });
    actRefLines_->setCheckable(true);
    actRefLines_->setChecked(AppSettings::instance().referenceLines());
    actOverlays_ = make(tr("Anotações na imagem (ON/OFF)"), "annotations", QKeySequence(tr("O")), [this] {
        grid_->setOverlaysVisible(actOverlays_->isChecked());
        AppSettings::instance().setOverlaysVisible(actOverlays_->isChecked());
    });
    actOverlays_->setCheckable(true);
    actOverlays_->setChecked(AppSettings::instance().overlaysVisible());
    actMpr_ = make(tr("MPR"), "mpr", QKeySequence(tr("Ctrl+M")), [this] { toggleMpr(actMpr_->isChecked()); },
                   tr("Reconstrução multiplanar (axial, coronal e sagital)  (Ctrl+M)"));
    actMpr_->setCheckable(true);
    act3d_ = make(tr("3D"), "cube", QKeySequence(), [] {},
                  tr("Volume rendering 3D — previsto para a versão 1.0 (módulo VTK)"));
    act3d_->setEnabled(false);
    actCine_ = make(tr("Cine"), "cine", QKeySequence(tr("Space")), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->setCinePlaying(!vp->cinePlaying());
        }
    });
    actUndo_ = make(tr("Desfazer"), "undo", QKeySequence::Undo, [this] { annotations_->undoStack()->undo(); });
    actUndo_->setEnabled(false);
    actRedo_ = make(tr("Refazer"), "undo", QKeySequence::Redo, [this] { annotations_->undoStack()->redo(); });
    actRedo_->setEnabled(false);
    actDelete_ = make(tr("Excluir medida selecionada"), "delete", QKeySequence::Delete, [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->deleteSelectedAnnotation();
        }
    });
    actDeleteAll_ = make(tr("Excluir todas as medidas"), "delete", QKeySequence(tr("Ctrl+Shift+Del")), [this] {
        if (annotations_->totalCount() > 0 &&
            QMessageBox::question(this, tr("Excluir medidas"), tr("Excluir todas as medidas desta sessão?")) ==
                QMessageBox::Yes) {
            annotations_->clearAll();
        }
    });
    actHistogram_ = make(tr("Histograma da ROI…"), "roi-rect", QKeySequence(tr("Ctrl+H")), [this] { showRoiHistogram(); });
    actInfo_ = make(tr("Informações DICOM…"), "info", QKeySequence(tr("Ctrl+I")), [this] { showDicomInfo(); });
    actVoiLut_ = make(tr("VOI LUT do arquivo"), "window-level", QKeySequence(), [this] {
        if (auto* vp = grid_->activeViewport()) {
            vp->setVoiLut(!vp->voiLutActive());
        }
    });

    // Window presets 1..8 (CT) via number keys.
    for (int i = 0; i < static_cast<int>(ctPresets().size()) && i < 9; ++i) {
        auto* a = new QAction(this);
        a->setShortcut(QKeySequence(Qt::Key_1 + i));
        connect(a, &QAction::triggered, this, [this, i] {
            auto* vp = grid_->activeViewport();
            if (vp && vp->source() && vp->source()->isCt()) {
                applyPreset(ctPresets()[static_cast<size_t>(i)]);
            }
        });
        addAction(a);
    }
    auto* next = new QAction(this);
    next->setShortcut(Qt::Key_Tab);
    connect(next, &QAction::triggered, grid_, &ViewerGrid::activateNext);
    addAction(next);
    auto* fullscreen = new QAction(this);
    fullscreen->setShortcut(QKeySequence::FullScreen);
    connect(fullscreen, &QAction::triggered, this, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    addAction(fullscreen);
    // Oblique MPR: rotate the other two planes around the active view.
    for (int sign : {-1, 1}) {
        auto* a = new QAction(this);
        a->setShortcut(sign < 0 ? QKeySequence(tr("Ctrl+[")) : QKeySequence(tr("Ctrl+]")));
        connect(a, &QAction::triggered, this, [this, sign] {
            auto* ms = mprOf(grid_->activeViewport());
            if (ms == nullptr) {
                return;
            }
            for (auto o : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
                if (o != ms->orientation()) {
                    ms->session()->rotate(o, ms->orientation(), 5.0 * sign);
                }
            }
        });
        addAction(a);
    }
}

void MainWindow::createMenus() {
    auto* file = menuBar()->addMenu(tr("&Arquivo"));
    file->addAction(actOpenFiles_);
    file->addAction(actOpenFolder_);
    file->addAction(actOpenArchive_);
    file->addSeparator();
    file->addAction(actExport_);
    file->addAction(actCapture_);
    file->addSeparator();
    file->addAction(tr("Fechar estudos"), this, &MainWindow::closeStudies);
    file->addSeparator();
    file->addAction(tr("Sair"), QKeySequence::Quit, this, &QWidget::close);

    auto* view = menuBar()->addMenu(tr("&Exibir"));
    layoutMenu_ = view->addMenu(Icons::get("layout"), tr("Layout"));
    const std::vector<std::pair<int, int>> layouts = {{1, 1}, {1, 2}, {2, 1}, {2, 2}, {3, 2}, {3, 3}};
    int k = 1;
    for (const auto& [r, c] : layouts) {
        auto* a = layoutMenu_->addAction(QStringLiteral("%1 × %2").arg(r).arg(c), this,
                                         [this, r = r, c = c] { grid_->setLayoutGrid(r, c); });
        a->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(k++)));
        addAction(a);
    }
    view->addAction(actStudies_);
    view->addSeparator();
    view->addAction(actOverlays_);
    view->addAction(actRefLines_);
    view->addAction(actSync_);
    view->addSeparator();
    view->addAction(actFit_);
    view->addAction(actActual_);
    view->addAction(actReset_);

    auto* tools = menuBar()->addMenu(tr("&Ferramentas"));
    for (Tool t : {Tool::WindowLevel, Tool::Pan, Tool::Zoom, Tool::Scroll, Tool::Probe, Tool::Crosshair}) {
        tools->addAction(toolActions_[t]);
    }

    auto* image = menuBar()->addMenu(tr("&Imagem"));
    presetMenu_ = image->addMenu(Icons::get("window-level"), tr("Presets de janela"));
    connect(presetMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildPresetMenu);
    image->addAction(actVoiLut_);
    image->addAction(actInvert_);
    image->addSeparator();
    image->addAction(actRotCw_);
    image->addAction(actRotCcw_);
    image->addAction(tr("Rotação livre…"), this, [this] {
        auto* vp = grid_->activeViewport();
        if (vp == nullptr) {
            return;
        }
        bool ok = false;
        const double deg = QInputDialog::getDouble(this, tr("Rotação livre"), tr("Ângulo adicional (graus):"),
                                                   vp->viewState().freeRotation, -360, 360, 1, &ok);
        if (ok) {
            vp->setFreeRotation(deg);
        }
    });
    image->addAction(actFlipH_);
    image->addAction(actFlipV_);
    image->addSeparator();
    image->addAction(actCine_);
    image->addAction(tr("Velocidade do cine…"), this, [this] {
        auto* vp = grid_->activeViewport();
        if (vp == nullptr) {
            return;
        }
        bool ok = false;
        const double fps = QInputDialog::getDouble(this, tr("Cine"), tr("Quadros por segundo:"), vp->cineFps(), 1,
                                                   120, 0, &ok);
        if (ok) {
            vp->setCineFps(fps);
        }
    });
    auto* loop = image->addAction(tr("Cine em loop"));
    loop->setCheckable(true);
    loop->setChecked(true);
    connect(loop, &QAction::toggled, this, [this](bool on) {
        for (auto* vp : grid_->visibleViewports()) {
            vp->setCineLoop(on);
        }
    });
    auto* reverse = image->addAction(tr("Cine em sentido reverso"));
    reverse->setCheckable(true);
    connect(reverse, &QAction::toggled, this, [this](bool on) {
        for (auto* vp : grid_->visibleViewports()) {
            vp->setCineReverse(on);
        }
    });
    image->addSeparator();
    image->addAction(actInfo_);

    auto* measure = menuBar()->addMenu(tr("&Medidas"));
    measureMenu_ = new QMenu(tr("Medidas"), this);
    for (Tool t : {Tool::Distance, Tool::Angle, Tool::Cobb}) {
        measure->addAction(toolActions_[t]);
        measureMenu_->addAction(toolActions_[t]);
    }
    roiMenu_ = new QMenu(tr("ROI"), this);
    measure->addSeparator();
    for (Tool t : {Tool::RectRoi, Tool::EllipseRoi, Tool::FreehandRoi}) {
        measure->addAction(toolActions_[t]);
        roiMenu_->addAction(toolActions_[t]);
    }
    measure->addAction(toolActions_[Tool::Probe]);
    measure->addSeparator();
    measure->addAction(actHistogram_);
    measure->addSeparator();
    measure->addAction(actUndo_);
    measure->addAction(actRedo_);
    measure->addAction(actDelete_);
    measure->addAction(actDeleteAll_);

    auto* mpr = menuBar()->addMenu(tr("M&PR"));
    mpr->addAction(actMpr_);
    slabMenu_ = mpr->addMenu(tr("Espessura (thick slab)"));
    auto* slabGroup = new QActionGroup(this);
    for (double t : {0.0, 1.0, 2.0, 3.0, 5.0, 10.0, 20.0, 50.0}) {
        auto* a = slabMenu_->addAction(t == 0.0 ? tr("Desligado (plano fino)") : QStringLiteral("%1 mm").arg(t));
        a->setCheckable(true);
        a->setChecked(t == 0.0);
        slabGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { setSlab(t, -1); });
    }
    slabMenu_->addAction(tr("Personalizada…"), this, [this] {
        bool ok = false;
        const double t = QInputDialog::getDouble(this, tr("Espessura"), tr("Espessura do slab (mm):"), 5.0, 0.1, 500.0,
                                                 1, &ok);
        if (ok) {
            setSlab(t, -1);
        }
    });
    auto* modeGroup = new QActionGroup(this);
    mpr->addSeparator();
    const std::vector<std::pair<QString, int>> modes = {{tr("MIP (intensidade máxima)"), 1},
                                                        {tr("MinIP (intensidade mínima)"), 2},
                                                        {tr("Média"), 0}};
    for (const auto& [name, mode] : modes) {
        auto* a = mpr->addAction(name);
        a->setCheckable(true);
        a->setChecked(mode == 1);
        modeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode = mode] { setSlab(-1.0, mode); });
    }
    mpr->addSeparator();
    mpr->addAction(tr("Girar planos −5° (Ctrl+[)"), this, [this] {
        if (auto* ms = mprOf(grid_->activeViewport())) {
            for (auto o : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
                if (o != ms->orientation()) {
                    ms->session()->rotate(o, ms->orientation(), -5.0);
                }
            }
        }
    });
    mpr->addAction(tr("Girar planos +5° (Ctrl+])"), this, [this] {
        if (auto* ms = mprOf(grid_->activeViewport())) {
            for (auto o : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
                if (o != ms->orientation()) {
                    ms->session()->rotate(o, ms->orientation(), 5.0);
                }
            }
        }
    });
    mpr->addAction(tr("Restaurar planos ortogonais"), this, [this] {
        if (grid_->mprSession()) {
            grid_->mprSession()->resetOrientation();
        }
    });

    auto* help = menuBar()->addMenu(tr("A&juda"));
    help->addAction(tr("Atalhos de teclado"), this, &MainWindow::showShortcuts);
    help->addAction(tr("Preferências…"), QKeySequence::Preferences, this, &MainWindow::showPreferences);
    help->addAction(tr("Local do arquivo de log"), this, [this] {
        QMessageBox::information(this, tr("Log"),
                                 QString::fromStdString(Logger::instance().filePath().string()));
    });
    help->addSeparator();
    help->addAction(tr("Sobre o VisualTC"), this, &MainWindow::showAbout);
}

void MainWindow::createToolbar() {
    auto* tb = addToolBar(tr("Ferramentas"));
    tb->setObjectName("mainToolbar");
    tb->setMovable(false);
    tb->setIconSize(QSize(22, 22));
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    actOpenFiles_->setIconText(tr("Abrir"));
    actOpenFolder_->setIconText(tr("Pasta"));
    tb->addAction(actOpenFiles_);
    tb->addAction(actOpenFolder_);
    tb->addAction(actStudies_);

    auto* layoutBtn = new QToolButton(tb);
    layoutBtn->setIcon(Icons::get("layout"));
    layoutBtn->setText(tr("Layout"));
    layoutBtn->setToolTip(tr("Layout dos viewports (Ctrl+1…Ctrl+6)"));
    layoutBtn->setMenu(layoutMenu_);
    layoutBtn->setPopupMode(QToolButton::InstantPopup);
    layoutBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->addWidget(layoutBtn);
    tb->addSeparator();

    tb->addAction(toolActions_[Tool::Zoom]);
    tb->addAction(toolActions_[Tool::Pan]);
    auto* wlBtn = new QToolButton(tb);
    wlBtn->setDefaultAction(toolActions_[Tool::WindowLevel]);
    wlBtn->setMenu(presetMenu_);
    wlBtn->setPopupMode(QToolButton::MenuButtonPopup);
    wlBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->addWidget(wlBtn);
    tb->addAction(toolActions_[Tool::Scroll]);

    auto makeMenuButton = [tb](QMenu* menu, QAction* def, const QString& text) {
        auto* b = new QToolButton(tb);
        b->setDefaultAction(def);
        b->setMenu(menu);
        b->setPopupMode(QToolButton::MenuButtonPopup);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        QObject::connect(menu, &QMenu::triggered, b, [b, text](QAction* a) {
            b->setDefaultAction(a);
            Q_UNUSED(text);
        });
        tb->addWidget(b);
        return b;
    };
    toolActions_[Tool::Distance]->setIconText(tr("Medidas"));
    toolActions_[Tool::RectRoi]->setIconText(tr("ROI"));
    makeMenuButton(measureMenu_, toolActions_[Tool::Distance], tr("Medidas"));
    makeMenuButton(roiMenu_, toolActions_[Tool::RectRoi], tr("ROI"));
    toolActions_[Tool::Probe]->setIconText(tr("Valor"));
    tb->addAction(toolActions_[Tool::Probe]);
    tb->addSeparator();
    tb->addAction(actMpr_);
    toolActions_[Tool::Crosshair]->setIconText(tr("Cruz"));
    tb->addAction(toolActions_[Tool::Crosshair]);
    tb->addAction(act3d_);
    tb->addSeparator();
    actRotCw_->setIconText(tr("Girar"));
    actFlipH_->setIconText(tr("Espelhar"));
    tb->addAction(actRotCw_);
    tb->addAction(actFlipH_);
    tb->addAction(actInvert_);
    tb->addAction(actSync_);
    actRefLines_->setIconText(tr("Ref."));
    tb->addAction(actRefLines_);
    actOverlays_->setIconText(tr("Anotações"));
    tb->addAction(actOverlays_);
    tb->addAction(actCine_);
    tb->addSeparator();
    actCapture_->setIconText(tr("Capturar"));
    actExport_->setIconText(tr("Exportar"));
    tb->addAction(actCapture_);
    tb->addAction(actExport_);
    tb->addAction(actReset_);
    auto* prefs = new QAction(Icons::get("settings"), tr("Preferências"), this);
    connect(prefs, &QAction::triggered, this, &MainWindow::showPreferences);
    tb->addAction(prefs);
}

void MainWindow::createDock() {
    browser_ = new SeriesBrowser(this);
    browserDock_ = new QDockWidget(tr("Estudos e séries"), this);
    browserDock_->setObjectName("seriesDock");
    browserDock_->setWidget(browser_);
    browserDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    browserDock_->setMinimumWidth(250);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock_);
    connect(browser_, &SeriesBrowser::seriesActivated, this, [this](const QString& id) { openSeries(id); });
    connect(browser_, &SeriesBrowser::seriesMprRequested, this, [this](const QString& id) { startMpr(id); });
    connect(browser_, &SeriesBrowser::seriesInfoRequested, this, [this](const QString& id) { showDicomInfo(id); });
}

void MainWindow::createStatusBar() {
    cursorLabel_ = new QLabel(this);
    cursorLabel_->setMinimumWidth(320);
    infoLabel_ = new QLabel(this);
    cacheLabel_ = new QLabel(this);
    progress_ = new QProgressBar(this);
    progress_->setMaximumWidth(220);
    progress_->setMaximumHeight(14);
    progress_->hide();
    cancelButton_ = new QToolButton(this);
    cancelButton_->setText(tr("Cancelar"));
    cancelButton_->hide();
    connect(cancelButton_, &QToolButton::clicked, this, [this] {
        import_->cancel();
        mprCancel_ = true;
    });
    statusBar()->addWidget(cursorLabel_, 1);
    statusBar()->addPermanentWidget(infoLabel_);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->addPermanentWidget(cancelButton_);
    statusBar()->addPermanentWidget(cacheLabel_);
    if (!DecoderClient::isolationActive()) {
        auto* warn = new QLabel(tr("decodificação no próprio processo"), this);
        warn->setToolTip(tr("O decodificador isolado não está ativo: um arquivo corrompido pode encerrar o programa."));
        warn->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::colors().warning.name()));
        statusBar()->addPermanentWidget(warn);
    }
}

// --------------------------------------------------------------- import

namespace {
// DICOM files often have no extension, so "all files" stays the default;
// archives are recognised by content anyway, the filter only helps browsing.
const char* kArchiveFilter = QT_TRANSLATE_NOOP("MainWindow",
                                               "Exames compactados (*.zip *.rar *.7z *.tar *.tgz *.gz *.bz2 *.xz "
                                               "*.zst *.iso *.ZIP *.RAR *.7Z *.ISO)");
}  // namespace

void MainWindow::openFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Abrir arquivos DICOM ou exames compactados"), AppSettings::instance().lastOpenDirectory(),
        tr("Todos os arquivos (*)") + ";;" + tr(kArchiveFilter));
    if (!files.isEmpty()) {
        AppSettings::instance().setLastOpenDirectory(QFileInfo(files.front()).absolutePath());
        importPaths(files);
    }
}

void MainWindow::openArchive() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Abrir exame compactado"), AppSettings::instance().lastOpenDirectory(),
        tr(kArchiveFilter) + ";;" + tr("Todos os arquivos (*)"));
    if (!files.isEmpty()) {
        AppSettings::instance().setLastOpenDirectory(QFileInfo(files.front()).absolutePath());
        importPaths(files);
    }
}

void MainWindow::openFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Abrir pasta com exames DICOM"),
                                                          AppSettings::instance().lastOpenDirectory());
    if (!dir.isEmpty()) {
        AppSettings::instance().setLastOpenDirectory(dir);
        importPaths({dir});
    }
}

void MainWindow::importPaths(const QStringList& paths) {
    if (paths.isEmpty()) {
        return;
    }
    if (import_->running()) {
        statusBar()->showMessage(tr("Aguarde a importação em andamento terminar."), 4000);
        return;
    }
    progress_->setRange(0, 0);
    progress_->show();
    cancelButton_->show();
    infoLabel_->setText(tr("Procurando arquivos DICOM…"));
    const std::filesystem::path extractRoot =
        extraction_ && extraction_->valid() ? extraction_->newImportDir() : std::filesystem::path();
    import_->start(paths, extractRoot);
}

void MainWindow::onImportProgress(int processed, int total, int found, const QString& archive) {
    if (!archive.isEmpty()) {
        progress_->setRange(0, 0);  // busy indicator while the archive is expanded
        infoLabel_->setText(tr("Extraindo %1…").arg(archive));
        return;
    }
    progress_->setRange(0, std::max(1, total));
    progress_->setValue(processed);
    infoLabel_->setText(tr("Lendo %1 de %2 arquivos · %3 imagens").arg(processed).arg(total).arg(found));
}

void MainWindow::onImportFinished() {
    progress_->hide();
    cancelButton_->hide();
    if (discardImport_) {
        discardImport_ = false;
        infoLabel_->setText(tr("Importação cancelada."));
        return;
    }
    const ScanResult& r = import_->result();
    importIssues_.insert(importIssues_.end(), r.issues.begin(), r.issues.end());
    displayNames_.insert(r.displayNames.begin(), r.displayNames.end());
    const std::size_t added = db_.addInstances(r.instances);
    browser_->setDatabase(db_);
    updateEmptyHint();
    for (const auto& s : db_.allSeries()) {
        thumbs_->request(s);
    }
    QString msg = tr("%1 imagens importadas em %2 séries.").arg(added).arg(db_.allSeries().size());
    if (r.archivesOpened > 0) {
        msg += " " + tr("%n arquivo(s) compactado(s) aberto(s).", "", static_cast<int>(r.archivesOpened));
    }
    if (r.nonDicomFiles > 0) {
        msg += " " + tr("%1 arquivos não-DICOM ignorados.").arg(r.nonDicomFiles);
    }
    if (!r.issues.empty()) {
        msg += " " + tr("%1 arquivos com problemas (menu Arquivo › Problemas de importação).").arg(r.issues.size());
    }
    if (r.cancelled) {
        msg = tr("Importação cancelada. ") + msg;
    }
    if (r.instances.empty() && r.filesVisited > 0 && r.issues.empty()) {
        msg = tr("Nenhuma imagem DICOM encontrada em %1 arquivos.").arg(r.filesVisited);
    }
    infoLabel_->setText(msg);
    if (!r.issues.empty()) {
        if (issuesAction_ == nullptr) {
            auto* fileMenu = menuBar()->actions().front()->menu();
            issuesAction_ = new QAction(tr("Problemas de importação…"), this);
            connect(issuesAction_, &QAction::triggered, this, &MainWindow::showImportIssues);
            const auto actions = fileMenu->actions();
            fileMenu->insertAction(actions.size() > 3 ? actions.at(3) : nullptr, issuesAction_);
        }
        statusBar()->showMessage(msg, 10000);
    }

    // Open something useful right away if nothing is displayed.
    bool anyShown = false;
    for (auto* vp : grid_->visibleViewports()) {
        anyShown = anyShown || vp->source() != nullptr;
    }
    if (!anyShown) {
        // Largest series that is not a scout/localizer; a localizer only when
        // nothing else exists.
        SeriesPtr best;
        bool bestIsLocalizer = true;
        for (const auto& s : db_.allSeries()) {
            const bool localizer = s->frameCount() <= 3 || s->firstInstance().hasLocalizerImageType;
            const bool better = !best || (bestIsLocalizer && !localizer) ||
                                (localizer == bestIsLocalizer && s->frameCount() > best->frameCount());
            if (better) {
                best = s;
                bestIsLocalizer = localizer;
            }
        }
        if (best) {
            openSeries(QString::fromStdString(best->id));
        }
    }
    if (!automation_.screenshot.isEmpty() && automationStage_ == 0) {
        automationStage_ = 1;
        QTimer::singleShot(100, this, &MainWindow::runAutomationStep);
    }
}

void MainWindow::updateEmptyHint() {
    if (db_.allSeries().empty()) {
        grid_->setEmptyHint(tr("Para abrir um exame, arraste para cá a pasta do CD/pendrive ou o arquivo "
                               "compactado (ZIP, RAR, 7z) — ou use Abrir / Pasta na barra de ferramentas."));
    } else {
        grid_->setEmptyHint(QString());  // default: drag a series from the list
    }
}

void MainWindow::showImportIssues() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Problemas de importação"));
    dlg.resize(820, 420);
    auto* layout = new QVBoxLayout(&dlg);
    auto* tree = new QTreeWidget(&dlg);
    tree->setHeaderLabels({tr("Arquivo"), tr("Motivo")});
    tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree->setColumnWidth(0, 380);
    for (const auto& issue : importIssues_) {
        new QTreeWidgetItem(tree, {QString::fromStdString(issue.filePath), QString::fromStdString(issue.message)});
    }
    layout->addWidget(tree);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
}

void MainWindow::closeStudies() {
    if (mprBuilding_) {
        mprCancel_ = true;
    }
    // An import still running would add its images to the emptied list.
    import_->cancel();
    discardImport_ = import_->running();
    importIssues_.clear();
    displayNames_.clear();
    grid_->clearAll();
    annotations_->clearAll();
    annotations_->undoStack()->clear();
    db_.clear();
    browser_->setDatabase(db_);
    updateEmptyHint();
    thumbs_->clear();
    frames_->clear();
    if (extraction_ && !import_->running()) {
        extraction_->clear();  // images extracted from compressed exams
    }
    actMpr_->setChecked(false);
    infoLabel_->clear();
    updateActionStates();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths << url.toLocalFile();
        }
    }
    if (!paths.isEmpty()) {
        importPaths(paths);
        event->acceptProposedAction();
    }
}

// ------------------------------------------------------------- viewing

void MainWindow::openSeries(const QString& seriesId, Viewport* target) {
    const SeriesPtr series = db_.findSeries(seriesId.toStdString());
    if (!series) {
        return;
    }
    Viewport* vp = target != nullptr ? target : grid_->activeViewport();
    if (grid_->mprSession() && mprOf(vp) != nullptr && target == nullptr) {
        // Opening a series from the browser leaves MPR mode.
        grid_->exitMpr();
        actMpr_->setChecked(false);
        vp = grid_->activeViewport();
    }
    auto source = std::make_shared<StackSource>(series, frames_, AppSettings::instance().preloadSeries());
    grid_->showSource(vp, source);
    grid_->setActive(vp);
    updateActionStates();
    updateStatusInfo();
}

QString MainWindow::activeSeriesId() const {
    auto* vp = grid_->activeViewport();
    if (auto* st = stackOf(vp)) {
        return QString::fromStdString(st->series()->id);
    }
    if (auto* ms = mprOf(vp)) {
        return ms->session()->series() ? QString::fromStdString(ms->session()->series()->id) : QString();
    }
    return {};
}

void MainWindow::toggleMpr(bool on) {
    if (on) {
        startMpr();
    } else {
        mprCancel_ = true;
        grid_->exitMpr();
        updateActionStates();
    }
}

void MainWindow::startMpr(const QString& seriesId) {
    if (mprBuilding_) {
        return;
    }
    const QString id = seriesId.isEmpty() ? activeSeriesId() : seriesId;
    const SeriesPtr series = db_.findSeries(id.toStdString());
    if (!series) {
        actMpr_->setChecked(false);
        statusBar()->showMessage(tr("Abra uma série volumétrica (TC ou RM) antes de iniciar o MPR."), 6000);
        return;
    }
    if (!series->geometry.volumetric) {
        actMpr_->setChecked(false);
        QStringList reasons;
        for (auto issue : series->geometry.issues) {
            reasons << "• " + QString::fromStdString(describe(issue));
        }
        QMessageBox::information(this, tr("MPR"),
                                 tr("Não foi possível construir um volume espacial consistente a partir desta "
                                    "série.") +
                                     (reasons.isEmpty() ? QString() : "\n\n" + reasons.join('\n')));
        return;
    }
    if (series->geometry.has(GeometryIssue::MissingSlices)) {
        statusBar()->showMessage(QString::fromStdString(describe(GeometryIssue::MissingSlices)), 10000);
    }
    if (mprThread_.joinable()) {
        mprThread_.join();
    }
    mprBuilding_ = true;
    mprCancel_ = false;
    actMpr_->setChecked(true);
    progress_->setRange(0, series->frameCount());
    progress_->setValue(0);
    progress_->show();
    cancelButton_->show();
    infoLabel_->setText(tr("Montando volume para MPR…"));
    const std::uint64_t ram = physicalMemoryBytes();
    const std::uint64_t maxBytes = ram > 0 ? std::max<std::uint64_t>(ram / 2, 512 * MiB) : 2048 * MiB;
    mprThread_ = std::thread([this, series, maxBytes] {
        // Multi-frame objects (Enhanced CT/MR) are decoded once and kept here
        // while their frames are copied: going through the cache frame by
        // frame would decode the whole file again whenever it does not fit.
        const InstanceInfo* memoInstance = nullptr;
        std::vector<DecodedFramePtr> memoFrames;
        auto fetch = [this, &memoInstance, &memoFrames](const FrameRef& r) -> DecodedFramePtr {
            if (auto f = frames_->cached(r)) {
                return f;
            }
            if (r.instance->numberOfFrames > 1) {
                if (memoInstance != r.instance.get()) {
                    memoFrames = frames_->decodeAllNow(r);
                    memoInstance = r.instance.get();
                }
                const auto idx = static_cast<std::size_t>(r.frame);
                return idx < memoFrames.size() ? memoFrames[idx] : nullptr;
            }
            return frames_->decodeNow(r);
        };
        auto progress = [this](int done, int total) {
            QMetaObject::invokeMethod(
                this,
                [this, done, total] {
                    progress_->setRange(0, total);
                    progress_->setValue(done);
                },
                Qt::QueuedConnection);
        };
        ImageVolume::BuildResult result = ImageVolume::build(*series, fetch, maxBytes, progress, &mprCancel_);
        DecoderClient::releaseThreadWorker();
        QMetaObject::invokeMethod(
            this,
            [this, result, series] {
                if (mprThread_.joinable()) {
                    mprThread_.join();
                }
                mprBuilding_ = false;
                progress_->hide();
                cancelButton_->hide();
                if (result.volume && !mprCancel_) {
                    auto session = std::make_shared<MprSession>(result.volume, series);
                    if (AppSettings::instance().quality() == QualityLevel::Performance) {
                        session->setInterpolation(Interpolation::Nearest);
                    }
                    grid_->enterMpr(session);
                    infoLabel_->setText(tr("MPR: %1 × %2 × %3 voxels · %4 MB")
                                            .arg(result.volume->nx())
                                            .arg(result.volume->ny())
                                            .arg(result.volume->nz())
                                            .arg(result.volume->byteSize() / MiB));
                    toolActions_[Tool::Crosshair]->setChecked(true);
                } else {
                    actMpr_->setChecked(false);
                    infoLabel_->clear();
                    if (!mprCancel_) {
                        QMessageBox::warning(this, tr("MPR"), QString::fromStdString(result.error));
                    }
                }
                updateActionStates();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::setSlab(double thickness, int mode) {
    auto session = grid_->mprSession();
    if (!session) {
        statusBar()->showMessage(tr("O thick slab se aplica ao MPR. Abra o MPR primeiro (Ctrl+M)."), 5000);
        return;
    }
    SlabParams p = session->slab();
    if (thickness >= 0.0) {
        p.thickness = thickness;
    }
    if (mode >= 0) {
        p.mode = static_cast<SlabMode>(mode);
    }
    session->setSlab(p);
}

void MainWindow::setTool(Tool tool) {
    grid_->setTool(tool);
    if (auto it = toolActions_.find(tool); it != toolActions_.end()) {
        it->second->setChecked(true);
    }
}

void MainWindow::applyPreset(const WindowPreset& preset) {
    if (auto* vp = grid_->activeViewport()) {
        vp->setWindow(preset.center, preset.width);
        statusBar()->showMessage(tr("Janela: %1 (WW %2 / WL %3)")
                                     .arg(QString::fromStdString(preset.name))
                                     .arg(preset.width)
                                     .arg(preset.center),
                                 3000);
    }
}

void MainWindow::rebuildPresetMenu() {
    presetMenu_->clear();
    auto* vp = grid_->activeViewport();
    if (vp == nullptr || !vp->source()) {
        presetMenu_->addAction(tr("(abra uma série)"))->setEnabled(false);
        return;
    }
    const FrameInfo* info = vp->source()->frameInfoAt(vp->sliceIndex());
    if (info != nullptr && !info->windows.empty()) {
        presetMenu_->addSection(tr("Do arquivo DICOM"));
        for (const auto& w : info->windows) {
            const QString name = w.explanation.empty() ? tr("Janela DICOM") : QString::fromStdString(w.explanation);
            presetMenu_->addAction(QStringLiteral("%1  (%2 / %3)").arg(name).arg(w.width).arg(w.center), this,
                                   [this, w] { applyPreset({w.explanation.empty() ? "DICOM" : w.explanation, w.center, w.width}); });
        }
    }
    const InstanceInfo* inst = vp->source()->instanceAt(vp->sliceIndex());
    if (inst != nullptr && inst->voiLut) {
        presetMenu_->addAction(actVoiLut_);
    }
    if (vp->source()->isCt()) {
        presetMenu_->addSection(tr("Tomografia (HU)"));
        int k = 1;
        for (const auto& p : ctPresets()) {
            auto* a = presetMenu_->addAction(QStringLiteral("%1  (%2 / %3)")
                                                 .arg(QString::fromStdString(p.name))
                                                 .arg(p.width)
                                                 .arg(p.center),
                                             this, [this, p] { applyPreset(p); });
            // Shown for reference only: keys 1-8 are window-wide actions
            // created once in createActions(). A second window-wide action
            // with the same key would make Qt treat the shortcut as
            // ambiguous and neither would fire.
            a->setShortcut(QKeySequence(Qt::Key_0 + k++));
            a->setShortcutContext(Qt::WidgetShortcut);
        }
    }
    const auto custom = AppSettings::instance().customPresets();
    if (!custom.isEmpty()) {
        presetMenu_->addSection(tr("Personalizados"));
        for (const auto& p : custom) {
            presetMenu_->addAction(QStringLiteral("%1  (%2 / %3)")
                                       .arg(QString::fromStdString(p.name))
                                       .arg(p.width)
                                       .arg(p.center),
                                   this, [this, p] { applyPreset(p); });
        }
    }
    presetMenu_->addSeparator();
    presetMenu_->addAction(tr("Automático (pela faixa de valores)"), this, [this] {
        if (auto* v = grid_->activeViewport(); v && v->currentFrame()) {
            applyPreset(autoWindow(*v->currentFrame()));
        }
    });
    presetMenu_->addAction(tr("Padrão do arquivo"), this, [this] {
        if (auto* v = grid_->activeViewport()) {
            v->resetWindowToDefault();
        }
    });
    presetMenu_->addAction(tr("Salvar janela atual como preset…"), this, &MainWindow::saveCurrentWindowAsPreset);
}

void MainWindow::saveCurrentWindowAsPreset() {
    auto* vp = grid_->activeViewport();
    if (vp == nullptr || !vp->hasWindow()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Novo preset"), tr("Nome do preset:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    auto presets = AppSettings::instance().customPresets();
    presets.push_back({name.trimmed().toStdString(), vp->windowCenter(), vp->windowWidth()});
    AppSettings::instance().setCustomPresets(presets);
}

void MainWindow::onActiveChanged() {
    updateActionStates();
    updateStatusInfo();
}

void MainWindow::updateActionStates() {
    auto* vp = grid_->activeViewport();
    const bool has = vp != nullptr && vp->source() != nullptr;
    for (auto* a : {actExport_, actCapture_, actInvert_, actRotCw_, actRotCcw_, actFlipH_, actFlipV_, actFit_,
                    actActual_, actReset_, actCine_, actInfo_}) {
        a->setEnabled(has);
    }
    actDelete_->setEnabled(has && vp->selectedAnnotation() != nullptr);
    actDeleteAll_->setEnabled(annotations_->totalCount() > 0);
    actHistogram_->setEnabled(has && vp->selectedAnnotation() != nullptr && vp->selectedAnnotation()->isRoi());
    actMpr_->setEnabled(has || grid_->mprSession() != nullptr);
    actMpr_->setChecked(grid_->mprSession() != nullptr || mprBuilding_);
    slabMenu_->setEnabled(grid_->mprSession() != nullptr);
    toolActions_[Tool::Crosshair]->setEnabled(grid_->mprSession() != nullptr);
    const InstanceInfo* inst = has ? vp->source()->instanceAt(vp->sliceIndex()) : nullptr;
    actVoiLut_->setEnabled(inst != nullptr && inst->voiLut.has_value());
}

void MainWindow::updateStatusInfo() {
    auto* vp = grid_->activeViewport();
    if (vp == nullptr || !vp->source() || import_->running() || mprBuilding_) {
        return;
    }
    const QLocale loc;
    QString text = vp->source()->seriesLabel();
    text += "  ·  " + tr("Imagem %1/%2").arg(vp->sliceIndex() + 1).arg(vp->source()->count());
    if (vp->hasWindow()) {
        text += "  ·  WW " + loc.toString(vp->windowWidth(), 'f', 0) + " WL " + loc.toString(vp->windowCenter(), 'f', 0);
    }
    infoLabel_->setText(text);
}

// --------------------------------------------------------------- dialogs

void MainWindow::showRoiHistogram() {
    auto* vp = grid_->activeViewport();
    if (vp == nullptr || !vp->selectedAnnotation() || !vp->selectedAnnotation()->isRoi()) {
        statusBar()->showMessage(tr("Selecione uma ROI para ver o histograma."), 4000);
        return;
    }
    const MeasureContext ctx = vp->measureContext();
    HistogramDialog dlg(vp->selectedAnnotation()->roiValues(ctx), ctx.unit, this);
    dlg.exec();
}

void MainWindow::showDicomInfo(const QString& seriesId) {
    SeriesPtr series;
    int frame = 0;
    if (!seriesId.isEmpty()) {
        series = db_.findSeries(seriesId.toStdString());
    } else if (auto* st = stackOf(grid_->activeViewport())) {
        series = st->series();
        frame = grid_->activeViewport()->sliceIndex();
    } else if (auto* ms = mprOf(grid_->activeViewport())) {
        series = ms->session()->series();
    }
    if (!series) {
        return;
    }
    DicomInfoDialog dlg(series, frame, this, [this](const std::string& path) {
        const auto it = displayNames_.find(path);
        return it != displayNames_.end() ? it->second : path;
    });
    dlg.exec();
}

void MainWindow::showPreferences() {
    PreferencesDialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) {
        const auto& s = AppSettings::instance();
        grid_->setSmooth(s.smoothInterpolation());
        grid_->setOverlaysVisible(s.overlaysVisible());
        actOverlays_->setChecked(s.overlaysVisible());
        grid_->setReferenceLinesEnabled(s.referenceLines());
        actRefLines_->setChecked(s.referenceLines());
        frames_->setCacheBudget(s.cacheMegabytes() > 0 ? static_cast<std::uint64_t>(s.cacheMegabytes()) * MiB
                                                       : defaultCacheBudgetBytes());
        DecoderClient::initialize(s.useIsolatedDecoder());
    }
}

void MainWindow::showShortcuts() {
    const QString text = tr(
        "<table cellpadding='3'>"
        "<tr><td><b>W</b></td><td>Window/Level</td><td><b>Z</b></td><td>Zoom</td></tr>"
        "<tr><td><b>P</b></td><td>Pan</td><td><b>S</b></td><td>Navegar cortes</td></tr>"
        "<tr><td><b>M</b></td><td>Régua</td><td><b>A</b></td><td>Ângulo</td></tr>"
        "<tr><td><b>C</b></td><td>Ângulo de Cobb</td><td><b>V</b></td><td>Valor do pixel</td></tr>"
        "<tr><td><b>R</b></td><td>ROI retangular</td><td><b>E</b></td><td>ROI elíptica</td></tr>"
        "<tr><td><b>L</b></td><td>ROI livre</td><td><b>X</b></td><td>Crosshair (MPR)</td></tr>"
        "<tr><td><b>I</b></td><td>Inverter</td><td><b>F</b></td><td>Ajustar à janela</td></tr>"
        "<tr><td><b>Shift+F</b></td><td>Tamanho real 1:1</td><td><b>Ctrl+0</b></td><td>Reset</td></tr>"
        "<tr><td><b>[ ]</b></td><td>Girar 90°</td><td><b>H / Shift+H</b></td><td>Espelhar</td></tr>"
        "<tr><td><b>Espaço</b></td><td>Cine play/pause</td><td><b>Esc</b></td><td>Ferramenta padrão</td></tr>"
        "<tr><td><b>1 … 8</b></td><td>Presets de TC</td><td><b>O</b></td><td>Anotações ON/OFF</td></tr>"
        "<tr><td><b>Y</b></td><td>Sincronizar</td><td><b>Ctrl+L</b></td><td>Linhas de referência</td></tr>"
        "<tr><td><b>Ctrl+1…6</b></td><td>Layouts</td><td><b>Tab</b></td><td>Próximo viewport</td></tr>"
        "<tr><td><b>Ctrl+M</b></td><td>MPR</td><td><b>Ctrl+[ ]</b></td><td>MPR oblíquo ±5°</td></tr>"
        "<tr><td><b>↑ ↓ PgUp PgDn</b></td><td>Navegar cortes</td><td><b>Home/End</b></td><td>Primeiro/último</td></tr>"
        "<tr><td><b>Ctrl+Z</b></td><td>Desfazer</td><td><b>Delete</b></td><td>Excluir medida</td></tr>"
        "<tr><td><b>Ctrl+E</b></td><td>Exportar</td><td><b>Ctrl+Shift+C</b></td><td>Capturar</td></tr>"
        "</table>"
        "<p>Mouse: roda navega · Ctrl+roda zoom · botão do meio pan · botão direito zoom · "
        "Shift+arrastar pan · duplo clique maximiza/restaura.</p>");
    QMessageBox box(this);
    box.setWindowTitle(tr("Atalhos de teclado"));
    box.setTextFormat(Qt::RichText);
    box.setText(text);
    box.exec();
}

void MainWindow::showAbout() {
    QMessageBox::about(
        this, tr("Sobre o VisualTC"),
        tr("<h3>VisualTC — DICOM Medical Image Viewer</h3>"
           "<p>Versão %1</p>"
           "<p>Visualizador DICOM desktop para TC, RM e outras modalidades, com funcionamento integralmente "
           "offline. Nenhum dado de paciente é enviado a servidores externos.</p>"
           "<p>Qt %2 · GDCM %3</p>"
           "<p><b>Aviso:</b> esta versão não é um dispositivo médico registrado (ANVISA/FDA/CE). Medidas e "
           "reconstruções devem ser conferidas antes de qualquer uso diagnóstico.</p>"
           "<p>Licenças de terceiros: THIRD_PARTY_LICENSES.md</p>")
            .arg(QStringLiteral(VISUALTC_VERSION), QString::fromLatin1(qVersion()),
                 QString::fromStdString(gdcmVersion())));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    AppSettings::instance().saveWindow(saveGeometry(), saveState());
    mprCancel_ = true;
    import_->cancel();
    QMainWindow::closeEvent(event);
}

// ------------------------------------------------------------ automation

void MainWindow::setAutomation(const AutomationOptions& options) { automation_ = options; }

void MainWindow::runAutomationStep() {
    switch (automationStage_) {
        case 1: {
            if (automation_.rows > 0 && automation_.cols > 0) {
                grid_->setLayoutGrid(automation_.rows, automation_.cols);
                const auto series = db_.allSeries();
                const auto visible = grid_->visibleViewports();
                for (size_t i = 0; i < visible.size() && i < series.size(); ++i) {
                    openSeries(QString::fromStdString(series[i]->id), visible[i]);
                }
            }
            if (automation_.sync) {
                actSync_->setChecked(true);
                grid_->setSyncEnabled(true);
            }
            if (automation_.mpr) {
                // Pick the largest volumetric series.
                SeriesPtr best;
                for (const auto& s : db_.allSeries()) {
                    if (s->geometry.volumetric && (!best || s->frameCount() > best->frameCount())) {
                        best = s;
                    }
                }
                if (best) {
                    startMpr(QString::fromStdString(best->id));
                }
            }
            automationStage_ = 2;
            break;
        }
        case 2:
            if (mprBuilding_ || frames_->pendingCount() > 0) {
                break;  // wait
            }
            if (automation_.slab > 0) {
                setSlab(automation_.slab, 1);
            }
            if (!automation_.preset.isEmpty()) {
                for (const auto& p : ctPresets()) {
                    if (QString::fromStdString(p.name) == automation_.preset) {
                        for (auto* vp : grid_->visibleViewports()) {
                            if (vp->source()) {
                                vp->setWindow(p.center, p.width);
                            }
                        }
                    }
                }
            }
            if (!automation_.demoMeasurements.isEmpty()) {
                auto* vp = grid_->activeViewport();
                if (vp && vp->currentFrame()) {
                    const double w = vp->currentFrame()->width;
                    const double h = vp->currentFrame()->height;
                    auto line = std::make_shared<Annotation>(AnnotationKind::Distance);
                    line->points = {{w * 0.30, h * 0.62}, {w * 0.70, h * 0.62}};
                    line->finished = true;
                    annotations_->add(vp->source()->annotationKey(vp->sliceIndex()), line);
                    auto roi = std::make_shared<Annotation>(AnnotationKind::Ellipse);
                    roi->points = {{w * 0.42, h * 0.36}, {w * 0.52, h * 0.46}};
                    roi->finished = true;
                    roi->labelOffset = QPointF(18, -40);
                    annotations_->add(vp->source()->annotationKey(vp->sliceIndex()), roi);
                    auto ang = std::make_shared<Annotation>(AnnotationKind::Angle);
                    ang->points = {{w * 0.20, h * 0.30}, {w * 0.28, h * 0.42}, {w * 0.36, h * 0.30}};
                    ang->finished = true;
                    annotations_->add(vp->source()->annotationKey(vp->sliceIndex()), ang);
                }
            }
            automationStage_ = 3;
            QTimer::singleShot(900, this, &MainWindow::runAutomationStep);
            return;
        case 3: {
            const QPixmap shot = grab();
            const bool ok = shot.save(automation_.screenshot);
            logInfo("automation", ok ? "Captura de tela salva." : "Falha ao salvar captura de tela.");
            automationStage_ = 4;
            QTimer::singleShot(100, qApp, &QCoreApplication::quit);
            return;
        }
        default:
            return;
    }
    QTimer::singleShot(200, this, &MainWindow::runAutomationStep);
}

}  // namespace vtc
