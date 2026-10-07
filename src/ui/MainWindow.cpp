#include "ui/MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileOpenEvent>
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
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>
#include <set>

#include "app/AppSettings.h"
#include "app/I18n.h"
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
#include "ui/SeriesDock.h"
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
    connect(grid_, &ViewerGrid::selectionChanged, this, &MainWindow::updateActionStates);
    connect(grid_, &ViewerGrid::cursorInfo, cursorLabel_, &QLabel::setText);
    connect(grid_, &ViewerGrid::seriesDropped, this,
            [this](Viewport* vp, const QString& id) { openSeries(id, vp); });
    connect(grid_, &ViewerGrid::displayedSeriesChanged, this, [this] {
        QSet<QString> ids;
        for (auto* vp : grid_->visibleViewports()) {
            if (const SeriesPtr series = seriesIn(vp, nullptr)) {
                ids.insert(QString::fromStdString(series->id));
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
    // Files the system asks us to open arrive one event each: gather the ones
    // that come together and import them as one exam (later, if an import is
    // still running).
    fileOpenTimer_ = new QTimer(this);
    fileOpenTimer_->setSingleShot(true);
    fileOpenTimer_->setInterval(250);
    connect(fileOpenTimer_, &QTimer::timeout, this, [this] {
        if (import_->running()) {
            fileOpenTimer_->start();
            return;
        }
        const QStringList paths = fileOpenQueue_;
        fileOpenQueue_.clear();
        importPaths(paths);
    });
    qApp->installEventFilter(this);

    restoreState(s.windowState());
    browserDock_->show();  // older versions let the panel be closed
    browserDock_->setExpandedWidth(s.seriesPanelWidth());
    browserDock_->setCollapsed(s.seriesPanelCollapsed());
    updateEmptyHint();
    setTool(Tool::WindowLevel);
    updateActionStates();
    logInfo("app", "Janela principal criada.");
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
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
        a->setProperty("vtcIcon", icon);  // redrawn when the highlight colour changes
        if (!key.isEmpty()) {
            a->setShortcut(key);
        }
        // Shortcut in the platform's own notation (⌘ on the Mac).
        const QString base = tip.isEmpty() ? text : tip;
        a->setToolTip(key.isEmpty() ? base : base + "  (" + key.toString(QKeySequence::NativeText) + ")");
        connect(a, &QAction::triggered, this, slot);
        addAction(a);
        return a;
    };

    actOpenFiles_ = make(tr("Abrir arquivos…"), "open-file", QKeySequence::Open, [this] { openFiles(); },
                         tr("Abrir arquivos DICOM ou exames compactados (ZIP, RAR, 7z, TAR, GZ, ISO)"));
    actOpenArchive_ = make(tr("Abrir exame compactado (ZIP, RAR, 7z)…"), "open-file", QKeySequence(),
                           [this] { openArchive(); });
    actOpenFolder_ = make(tr("Abrir pasta…"), "open-folder", QKeySequence(tr("Ctrl+Shift+O")), [this] { openFolder(); });
    actStudies_ = make(tr("Painel de séries"), "sidebar", QKeySequence(tr("F2")), [this] {
        browserDock_->show();
        browserDock_->toggleCollapsed();
        actStudies_->setChecked(!browserDock_->isCollapsed());
    }, tr("Mostrar ou recolher o painel de séries — recolhido, as imagens usam quase toda a largura da tela  (F2)"));
    actStudies_->setCheckable(true);
    actStudies_->setChecked(true);
    actExport_ = make(tr("Exportar imagem…"), "export", QKeySequence(tr("Ctrl+E")), [this] {
        ImageExporter::exportViewport(this, grid_->activeViewport());
    });
    actCapture_ = make(tr("Copiar imagem do viewport"), "capture", QKeySequence(tr("Ctrl+Shift+C")), [this] {
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
        {Tool::Scroll, tr("Rolar cortes"), "scroll", "S"},
        {Tool::Distance, tr("Régua"), "ruler", "M"},
        {Tool::Angle, tr("Ângulo"), "angle", "A"},
        {Tool::Cobb, tr("Ângulo de Cobb"), "cobb", "C"},
        {Tool::RectRoi, tr("ROI retangular"), "roi-rect", "R"},
        {Tool::EllipseRoi, tr("ROI elíptica"), "roi-ellipse", "E"},
        {Tool::FreehandRoi, tr("ROI livre"), "roi-freehand", "L"},
        {Tool::Probe, tr("Valor do pixel"), "probe", "V"},
        {Tool::Crosshair, tr("Posicionar o cruzamento (MPR)"), "crosshair", "Shift+X"},
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
        const bool on = actRefLines_->isChecked();
        grid_->setReferenceLinesEnabled(on);
        AppSettings::instance().setReferenceLines(on);
        if (on) {
            showReferenceLinesContext();
        }
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
                   tr("Reconstrução multiplanar (axial, coronal e sagital); a seta mostra MIP/MinIP, espessura e rotação"));
    actMpr_->setCheckable(true);
    // "Cruz": shows or hides the crosshair lines of the MPR.
    actCrosshairLines_ = make(tr("Linhas do MPR (cruz)"), "crosshair", QKeySequence(tr("X")), [this] {
        const bool on = actCrosshairLines_->isChecked();
        grid_->setMprGuidesVisible(on);
        AppSettings::instance().setMprCrosshairVisible(on);
        if (!grid_->mprSession()) {
            statusBar()->showMessage(on ? tr("As linhas aparecem no MPR (Ctrl+M).")
                                        : tr("As linhas do MPR ficarão ocultas."),
                                     5000);
        }
    }, tr("Mostrar ou ocultar as linhas coloridas (cruz) do MPR"));
    actCrosshairLines_->setCheckable(true);
    actCrosshairLines_->setChecked(AppSettings::instance().mprCrosshairVisible());
    grid_->setMprGuidesVisible(actCrosshairLines_->isChecked());
    // Plane of the series in the active viewport (reformatted from the volume).
    planeGroup_ = new QActionGroup(this);
    planeGroup_->setExclusive(true);
    for (auto o : {MprOrientation::Axial, MprOrientation::Sagittal, MprOrientation::Coronal}) {
        auto* a = new QAction(trCore(toLabel(o)), this);
        a->setCheckable(true);
        a->setData(static_cast<int>(o));
        planeGroup_->addAction(a);
        planeActions_[static_cast<int>(o)] = a;
        connect(a, &QAction::triggered, this, [this, o] { showPlane(o); });
    }
    actPlane_ = make(tr("Plano"), "plane", QKeySequence(tr("Ctrl+Shift+P")), [this] { cyclePlane(); },
                     tr("Mudar o plano da série ativa: axial → sagital → coronal"));
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
    // Not Ctrl+H: on macOS that is Cmd+H, which hides the application.
    actHistogram_ = make(tr("Histograma da ROI…"), "histogram", QKeySequence(tr("Ctrl+Shift+H")),
                         [this] { showRoiHistogram(); });
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
    const std::vector<std::pair<int, int>> layouts = {{1, 1}, {1, 2}, {1, 3}, {2, 1}, {2, 2}, {3, 2}, {3, 3}};
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
    view->addSeparator();
    // Highlight colour of checked buttons and of the patient name.
    auto* accentMenu = view->addMenu(tr("Cor de destaque"));
    accentGroup_ = new QActionGroup(this);
    accentGroup_->setExclusive(true);
    for (Accent a : kAccents) {
        QPixmap swatch(16, 16);
        swatch.fill(Theme::accentColor(a, true));
        auto* act = accentMenu->addAction(QIcon(swatch), Theme::accentName(a));
        act->setCheckable(true);
        act->setChecked(a == Theme::accent());
        act->setData(static_cast<int>(a));
        accentGroup_->addAction(act);
        connect(act, &QAction::triggered, this, [this, a] { setAccent(a); });
    }

    auto* tools = menuBar()->addMenu(tr("&Ferramentas"));
    for (Tool t : {Tool::WindowLevel, Tool::Pan, Tool::Zoom, Tool::Scroll, Tool::Probe, Tool::Crosshair}) {
        tools->addAction(toolActions_[t]);
    }

    auto* image = menuBar()->addMenu(tr("&Imagem"));
    presetMenu_ = image->addMenu(Icons::get("window-level"), tr("Presets de janela"));
    connect(presetMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildPresetMenu);
    // Colour tables (pseudo-colour) for greyscale images; the DICOM VOI LUT
    // of the file, when present, stays in the same menu.
    lutMenu_ = image->addMenu(Icons::get("lut"), tr("Tabela de cores (LUT)"));
    lutGroup_ = new QActionGroup(this);
    lutGroup_->setExclusive(true);
    for (ColorMap map : kColorMaps) {
        auto* a = lutMenu_->addAction(QIcon(colorMapSwatch(map)), trCore(colorMapName(map)));
        a->setCheckable(true);
        a->setChecked(map == ColorMap::Gray);
        a->setData(static_cast<int>(map));
        lutGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, map] { applyColorMap(map); });
    }
    lutMenu_->addSeparator();
    lutMenu_->addAction(actVoiLut_);
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
    roiMenu_->addSeparator();
    roiMenu_->addAction(actHistogram_);
    measure->addAction(toolActions_[Tool::Probe]);
    measure->addSeparator();
    measure->addAction(actHistogram_);
    measure->addSeparator();
    measure->addAction(actUndo_);
    measure->addAction(actRedo_);
    measure->addAction(actDelete_);
    measure->addAction(actDeleteAll_);

    // MPR options. The toolbar button has its own copy of this menu: on the
    // Mac a menu of the (native) menu bar is not reliable as a button popup,
    // and options live in the menu itself (no submenus).
    createMprActions();
    mprMenu_ = menuBar()->addMenu(tr("M&PR"));
    populateMprMenu(mprMenu_);
    connect(mprMenu_, &QMenu::aboutToShow, this, &MainWindow::syncMprMenu);

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
    actStudies_->setIconText(tr("Séries"));
    toolActions_[Tool::Scroll]->setIconText(tr("Rolar"));
    actSync_->setIconText(tr("Sincronizar"));
    tb->addAction(actOpenFiles_);
    tb->addAction(actOpenFolder_);
    tb->addAction(actStudies_);

    auto* layoutBtn = new QToolButton(tb);
    layoutBtn->setIcon(Icons::get("layout"));
    layoutBtn->setProperty("vtcIcon", QStringLiteral("layout"));
    layoutBtn->setText(tr("Layout"));
    layoutBtn->setToolTip(tr("Layout dos viewports (%1…%2)")
                              .arg(QKeySequence(tr("Ctrl+1")).toString(QKeySequence::NativeText),
                                   QKeySequence(tr("Ctrl+7")).toString(QKeySequence::NativeText)));
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
            if (a->isCheckable()) {  // tools become the button's action; commands (histogram) do not
                b->setDefaultAction(a);
            }
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
    // MPR: click opens/closes it; the arrow shows MIP/MinIP, thickness, rotation.
    mprToolbarMenu_ = new QMenu(tr("MPR"), this);
    populateMprMenu(mprToolbarMenu_);
    connect(mprToolbarMenu_, &QMenu::aboutToShow, this, &MainWindow::syncMprMenu);
    auto* mprBtn = new QToolButton(tb);
    mprBtn->setDefaultAction(actMpr_);
    mprBtn->setMenu(mprToolbarMenu_);
    mprBtn->setPopupMode(QToolButton::MenuButtonPopup);
    mprBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->addWidget(mprBtn);
    actCrosshairLines_->setIconText(tr("Cruz"));
    tb->addAction(actCrosshairLines_);
    // Plane: a click goes to the next plane, the arrow lists the three.
    auto* planeMenu = new QMenu(tr("Plano"), this);
    for (auto o : {MprOrientation::Axial, MprOrientation::Sagittal, MprOrientation::Coronal}) {
        planeMenu->addAction(planeActions_[static_cast<int>(o)]);
    }
    planeBtn_ = new QToolButton(tb);
    planeBtn_->setDefaultAction(actPlane_);
    planeBtn_->setMenu(planeMenu);
    planeBtn_->setPopupMode(QToolButton::MenuButtonPopup);
    planeBtn_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->addWidget(planeBtn_);
    tb->addSeparator();
    actRotCw_->setIconText(tr("Girar"));
    actFlipH_->setIconText(tr("Espelhar"));
    tb->addAction(actRotCw_);
    tb->addAction(actFlipH_);
    tb->addAction(actInvert_);
    auto* lutBtn = new QToolButton(tb);
    lutBtn->setIcon(Icons::get("lut"));
    lutBtn->setProperty("vtcIcon", QStringLiteral("lut"));
    lutBtn->setText(tr("LUT"));
    lutBtn->setToolTip(tr("Tabela de cores (LUT): tons de cinza, ferro quente, PET, arco-íris…"));
    lutBtn->setMenu(lutMenu_);
    lutBtn->setPopupMode(QToolButton::InstantPopup);
    lutBtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->addWidget(lutBtn);
    tb->addAction(actSync_);
    actRefLines_->setIconText(tr("Ref."));
    tb->addAction(actRefLines_);
    actOverlays_->setIconText(tr("Anotações"));
    tb->addAction(actOverlays_);
    tb->addAction(actCine_);
    tb->addSeparator();
    actExport_->setIconText(tr("Exportar"));
    tb->addAction(actExport_);
    auto* prefs = new QAction(Icons::get("settings"), tr("Preferências"), this);
    connect(prefs, &QAction::triggered, this, &MainWindow::showPreferences);
    tb->addAction(prefs);

    // Narrow screens (a 13" notebook): the least used buttons lose their text
    // first (icon and tooltip stay), so nothing has to hide behind "»".
    toolbar_ = tb;
    fitTimer_ = new QTimer(this);
    fitTimer_->setSingleShot(true);
    fitTimer_->setInterval(0);
    connect(fitTimer_, &QTimer::timeout, this, &MainWindow::fitToolbar);
    connect(toolGroup_, &QActionGroup::triggered, fitTimer_, qOverload<>(&QTimer::start));
    compactOrder_.clear();
    auto add = [this, tb](QObject* o) {
        QToolButton* b = qobject_cast<QToolButton*>(o);
        if (b == nullptr) {
            if (auto* a = qobject_cast<QAction*>(o)) {
                b = qobject_cast<QToolButton*>(tb->widgetForAction(a));
            }
        }
        if (b != nullptr) {
            compactOrder_.push_back(b);
        }
    };
    for (QObject* o : std::initializer_list<QObject*>{
             prefs, actExport_, actCine_, actOverlays_, actRefLines_, actSync_, lutBtn, actInvert_, actFlipH_,
             actRotCw_, actStudies_, actOpenFolder_, actOpenFiles_, layoutBtn, planeBtn_, actCrosshairLines_,
             mprBtn, toolActions_[Tool::Probe], toolActions_[Tool::Scroll], toolActions_[Tool::Pan],
             toolActions_[Tool::Zoom]}) {
        add(o);
    }
}

int MainWindow::toolbarWidthNeeded() const {
    int total = toolbar_->contentsMargins().left() + toolbar_->contentsMargins().right() + 12;
    const int spacing = toolbar_->layout() != nullptr ? std::max(0, toolbar_->layout()->spacing()) : 0;
    for (QAction* a : toolbar_->actions()) {
        if (!a->isVisible()) {
            continue;
        }
        if (QWidget* w = toolbar_->widgetForAction(a)) {
            total += w->sizeHint().width() + spacing;
        }
    }
    return total;
}

void MainWindow::fitToolbar() {
    if (toolbar_ == nullptr || fittingToolbar_) {
        return;
    }
    // Labels change (Plano: Sagital, a measurement tool chosen in a menu):
    // refit only when the width or a label is different.
    QString signature = QString::number(toolbar_->width());
    for (QAction* a : toolbar_->actions()) {
        if (auto* b = qobject_cast<QToolButton*>(toolbar_->widgetForAction(a))) {
            signature += QLatin1Char('|') + (b->defaultAction() != nullptr ? b->defaultAction()->iconText() : b->text());
        }
    }
    if (signature == lastFitSignature_) {
        return;
    }
    lastFitSignature_ = signature;
    fittingToolbar_ = true;
    for (auto* b : compactOrder_) {
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    }
    const int available = toolbar_->width();
    for (auto* b : compactOrder_) {
        if (toolbarWidthNeeded() <= available) {
            break;
        }
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    fittingToolbar_ = false;
}

void MainWindow::createDock() {
    browserDock_ = new SeriesDock(this);
    browser_ = browserDock_->browser();
    addDockWidget(Qt::LeftDockWidgetArea, browserDock_);
    connect(browserDock_, &SeriesDock::collapsedChanged, this, [this](bool collapsed) {
        actStudies_->setChecked(!collapsed);
    });
    // Collapsing replaces closing: keep the panel out of the toolbar's
    // right-click menu, where it could be hidden for good.
    browserDock_->toggleViewAction()->setVisible(false);
    connect(browser_, &SeriesBrowser::seriesActivated, this, [this](const QString& id) { openSeries(id); });
    connect(browser_, &SeriesBrowser::seriesMprRequested, this, [this](const QString& id) { startMpr(id); });
    connect(browser_, &SeriesBrowser::seriesInfoRequested, this, [this](const QString& id) { showDicomInfo(id); });
    connect(browser_, &SeriesBrowser::studyCloseRequested, this, &MainWindow::closeStudy);
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
    browserDock_->setSeriesCount(browser_->seriesCount());
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
        new QTreeWidgetItem(tree, {QString::fromStdString(issue.filePath), trCore(issue.message)});
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
    browserDock_->setSeriesCount(browser_->seriesCount());
    updateEmptyHint();
    thumbs_->clear();
    frames_->clear();
    if (extraction_ && !import_->running()) {
        extraction_->clear();  // images extracted from compressed exams
    }
    actMpr_->setChecked(false);
    volumeCache_ = {};
    reformatSession_.reset();
    pendingSlab_.reset();
    infoLabel_->clear();
    updateActionStates();
}

void MainWindow::closeStudy(const QString& patientKey, const QString& studyKey) {
    // Everything that shows or holds this exam goes first.
    std::set<std::string> seriesIds;
    for (const auto& patient : db_.patients()) {
        if (QString::fromStdString(patient->key) != patientKey) {
            continue;
        }
        for (const auto& study : patient->studies) {
            if (QString::fromStdString(study->key) == studyKey) {
                for (const auto& series : study->series) {
                    seriesIds.insert(series->id);
                }
            }
        }
    }
    if (seriesIds.empty()) {
        return;
    }
    if (mprBuilding_ && seriesIds.count(buildingSeriesId_)) {
        mprCancel_ = true;  // a volume of this study was being built
    }
    if (auto session = grid_->mprSession(); session && session->series() && seriesIds.count(session->series()->id)) {
        grid_->exitMpr();
        actMpr_->setChecked(false);
    }
    for (auto* vp : findChildren<Viewport*>()) {
        if (const SeriesPtr s = seriesIn(vp, nullptr); s && seriesIds.count(s->id)) {
            vp->setSource(nullptr);
        }
    }
    if (seriesIds.count(volumeCache_.seriesId)) {
        volumeCache_ = {};
    }
    if (reformatSession_ && reformatSession_->series() && seriesIds.count(reformatSession_->series()->id)) {
        reformatSession_.reset();
    }
    // Images extracted from a compressed exam are deleted with it.
    const std::vector<InstancePtr> removed = db_.removeStudy(patientKey.toStdString(), studyKey.toStdString());
    if (extraction_ && extraction_->valid()) {
        const QString area = QDir::cleanPath(extraction_->sessionDir()) + QLatin1Char('/');
        for (const auto& inst : removed) {
            const QString path = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(inst->filePath)));
            if (path.startsWith(area)) {
                QFile::remove(path);
            }
            displayNames_.erase(inst->filePath);
        }
    }
    frames_->clear();  // decoded pixels of this exam leave the memory
    browser_->setDatabase(db_);
    browserDock_->setSeriesCount(browser_->seriesCount());
    updateEmptyHint();
    QSet<QString> ids;
    for (auto* vp : grid_->visibleViewports()) {
        if (const SeriesPtr series = seriesIn(vp, nullptr)) {
            ids.insert(QString::fromStdString(series->id));
        }
    }
    browser_->markDisplayed(ids);
    infoLabel_->setText(tr("Estudo fechado (%n imagem(ns)).", "", static_cast<int>(removed.size())));
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

void MainWindow::showReferenceLinesContext() {
    if (grid_->referenceLineCount() > 0) {
        statusBar()->showMessage(tr("Linhas de referência ativadas."), 4000);
        return;
    }
    auto* vp = grid_->activeViewport();
    if (auto* ms = mprOf(vp); ms != nullptr && grid_->mprSession() && ms->session() == grid_->mprSession()) {
        statusBar()->showMessage(tr("No MPR, as linhas coloridas já mostram a posição de cada plano."), 6000);
        return;
    }
    const SeriesPtr active = seriesIn(vp, nullptr);
    const auto plane = vp != nullptr ? vp->currentPlane() : std::nullopt;
    if (!active || !plane || !plane->geometry.isSpatial()) {
        statusBar()->showMessage(tr("Esta imagem não tem posição no espaço (DICOM): não há linhas de referência."), 6000);
        return;
    }
    // Nothing comparable on screen: show, next to this image, the series of
    // the same study in another plane that best locates it (the scout first).
    const std::string studyUid = active->firstInstance().studyInstanceUid;
    const Vec3 n = plane->geometry.normal();
    SeriesPtr best;
    double bestScore = -1.0;
    for (const auto& patient : db_.patients()) {
        for (const auto& study : patient->studies) {
            if (study->studyInstanceUid != studyUid) {
                continue;
            }
            for (const auto& s : study->series) {
                if (s == active || s->frames.empty()) {
                    continue;
                }
                const FrameGeometry& g = s->frames[s->frames.size() / 2].geometry();
                if (!g.isSpatial() || planesParallel(g.normal(), n)) {
                    continue;
                }
                const bool localizer = s->firstInstance().hasLocalizerImageType || s->frameCount() <= 3;
                const double score = (localizer ? 10.0 : 0.0) + (1.0 - std::abs(g.normal().dot(n)));
                if (score > bestScore) {
                    bestScore = score;
                    best = s;
                }
            }
        }
    }
    if (!best) {
        statusBar()->showMessage(tr("As linhas de referência aparecem quando outra série do mesmo exame, em outro "
                                    "plano (ex.: topograma), está aberta em outro quadro."),
                                 8000);
        return;
    }
    auto visible = grid_->visibleViewports();
    if (visible.size() < 2) {
        grid_->setLayoutGrid(1, 2);
        visible = grid_->visibleViewports();
    }
    Viewport* target = nullptr;
    for (auto* v : visible) {
        if (v != vp && !v->source()) {
            target = v;
            break;
        }
    }
    for (auto* v : visible) {
        if (target == nullptr && v != vp) {
            target = v;
        }
    }
    if (target == nullptr) {
        return;
    }
    openSeries(QString::fromStdString(best->id), target);
    grid_->setActive(vp);  // the lines show where the image being read is
    statusBar()->showMessage(tr("Linhas de referência: %1 aberta ao lado.")
                                 .arg(seriesDescription(*best).isEmpty()
                                          ? tr("série em outro plano")
                                          : seriesDescription(*best)),
                             6000);
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
        pendingSlab_.reset();
        grid_->exitMpr();
        updateActionStates();
    }
}

bool MainWindow::checkVolumetric(const SeriesPtr& series, const QString& purpose) {
    if (!series) {
        statusBar()->showMessage(tr("Abra uma série volumétrica (TC ou RM) antes de iniciar o MPR."), 6000);
        return false;
    }
    if (!series->geometry.volumetric) {
        QStringList reasons;
        for (auto issue : series->geometry.issues) {
            reasons << "• " + trCore(describe(issue));
        }
        QMessageBox::information(this, purpose,
                                 tr("Não foi possível construir um volume espacial consistente a partir desta "
                                    "série.") +
                                     (reasons.isEmpty() ? QString() : "\n\n" + reasons.join('\n')));
        return false;
    }
    return true;
}

void MainWindow::startMpr(const QString& seriesId) {
    if (mprBuilding_) {
        return;
    }
    const QString id = seriesId.isEmpty() ? activeSeriesId() : seriesId;
    const SeriesPtr series = db_.findSeries(id.toStdString());
    if (!checkVolumetric(series, tr("MPR"))) {
        actMpr_->setChecked(false);
        pendingSlab_.reset();
        return;
    }
    if (series->geometry.has(GeometryIssue::MissingSlices)) {
        statusBar()->showMessage(trCore(describe(GeometryIssue::MissingSlices)), 10000);
    }
    actMpr_->setChecked(true);
    buildVolume(series, [this, series](const VolumePtr& volume, const QString& error) {
        if (!volume) {
            actMpr_->setChecked(false);
            pendingSlab_.reset();
            if (!error.isEmpty()) {
                QMessageBox::warning(this, tr("MPR"), error);
            }
            updateActionStates();
            return;
        }
        auto session = std::make_shared<MprSession>(volume, series);
        if (AppSettings::instance().quality() == QualityLevel::Performance) {
            session->setInterpolation(Interpolation::Nearest);
        }
        grid_->enterMpr(session);
        infoLabel_->setText(tr("MPR: %1 × %2 × %3 voxels · %4 MB")
                                .arg(volume->nx())
                                .arg(volume->ny())
                                .arg(volume->nz())
                                .arg(volume->byteSize() / MiB));
        toolActions_[Tool::Crosshair]->setChecked(true);
        if (pendingSlab_) {  // MIP/MinIP or a thickness chosen before the MPR was open
            const auto [thickness, mode] = *pendingSlab_;
            pendingSlab_.reset();
            setSlab(thickness, mode);
        }
        updateActionStates();
    });
}

void MainWindow::buildVolume(const SeriesPtr& series, std::function<void(const VolumePtr&, const QString&)> done) {
    // The same volume serves the MPR and the plane button: built once per series.
    if (volumeCache_.volume && volumeCache_.seriesId == series->id) {
        done(volumeCache_.volume, QString());
        return;
    }
    if (auto session = grid_->mprSession(); session && session->series() == series) {
        done(session->volumePtr(), QString());
        return;
    }
    if (mprBuilding_) {
        statusBar()->showMessage(tr("Aguarde: um volume ainda está sendo montado."), 4000);
        return;
    }
    if (mprThread_.joinable()) {
        mprThread_.join();
    }
    volumeCache_ = {};  // one volume in memory at a time
    mprBuilding_ = true;
    buildingSeriesId_ = series->id;
    mprCancel_ = false;
    progress_->setRange(0, series->frameCount());
    progress_->setValue(0);
    progress_->show();
    cancelButton_->show();
    infoLabel_->setText(tr("Montando o volume da série…"));
    const std::uint64_t ram = physicalMemoryBytes();
    const std::uint64_t maxBytes = ram > 0 ? std::max<std::uint64_t>(ram / 2, 512 * MiB) : 2048 * MiB;
    mprThread_ = std::thread([this, series, maxBytes, done = std::move(done)] {
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
        auto progress = [this](int doneCount, int total) {
            QMetaObject::invokeMethod(
                this,
                [this, doneCount, total] {
                    progress_->setRange(0, total);
                    progress_->setValue(doneCount);
                },
                Qt::QueuedConnection);
        };
        ImageVolume::BuildResult result = ImageVolume::build(*series, fetch, maxBytes, progress, &mprCancel_);
        DecoderClient::releaseThreadWorker();
        QMetaObject::invokeMethod(
            this,
            [this, result, series, done] {
                if (mprThread_.joinable()) {
                    mprThread_.join();
                }
                mprBuilding_ = false;
                progress_->hide();
                cancelButton_->hide();
                infoLabel_->clear();
                if (result.volume && !mprCancel_) {
                    volumeCache_ = {series->id, result.volume};
                    done(result.volume, QString());
                } else {
                    done(nullptr, mprCancel_ ? QString() : trCore(result.error));
                }
            },
            Qt::QueuedConnection);
    });
}

MprOrientation MainWindow::nativeOrientation(const SeriesPtr& series) {
    // Plane in which the images were acquired: the axis closest to the normal.
    const Vec3 n = series->frames[series->frames.size() / 2].geometry().normal();
    const double ax = std::abs(n.x);
    const double ay = std::abs(n.y);
    const double az = std::abs(n.z);
    if (az >= ax && az >= ay) {
        return MprOrientation::Axial;
    }
    return ay >= ax ? MprOrientation::Coronal : MprOrientation::Sagittal;
}

SeriesPtr MainWindow::seriesIn(Viewport* vp, std::optional<MprOrientation>* plane) const {
    if (auto* st = stackOf(vp)) {
        if (plane != nullptr && st->series()->geometry.volumetric) {
            *plane = nativeOrientation(st->series());
        }
        return st->series();
    }
    if (auto* ms = mprOf(vp)) {
        if (plane != nullptr) {
            *plane = ms->orientation();
        }
        return ms->session()->series();
    }
    return nullptr;
}

void MainWindow::showPlane(MprOrientation o) {
    Viewport* vp = grid_->activeViewport();
    if (auto* ms = mprOf(vp); ms != nullptr && grid_->mprSession() && ms->session() == grid_->mprSession()) {
        statusBar()->showMessage(tr("No MPR os três planos já estão na tela. Feche o MPR para mudar o plano de uma "
                                    "série."),
                                 6000);
        updateActionStates();
        return;
    }
    std::optional<MprOrientation> current;
    const SeriesPtr series = seriesIn(vp, &current);
    if (!series) {
        statusBar()->showMessage(tr("Abra uma série para mudar o plano."), 4000);
        updateActionStates();
        return;
    }
    if (!series->geometry.volumetric) {
        statusBar()->showMessage(tr("Esta série não permite outros planos: são necessários cortes paralelos e "
                                    "coerentes (TC ou RM volumétrica)."),
                                 7000);
        updateActionStates();
        return;
    }
    if (current == o) {
        return;
    }
    if (o == nativeOrientation(series)) {
        // The acquired plane: the original images, not a reconstruction.
        openSeries(QString::fromStdString(series->id), vp);
        statusBar()->showMessage(tr("Plano %1: imagens originais.").arg(trCore(toLabel(o))), 4000);
        return;
    }
    QPointer<Viewport> target(vp);
    const auto shown = vp->source();
    buildVolume(series, [this, target, shown, series, o](const VolumePtr& volume, const QString& error) {
        if (!volume) {
            if (!error.isEmpty()) {
                QMessageBox::warning(this, tr("Plano"), error);
            }
            updateActionStates();
            return;
        }
        if (target.isNull() || target->source() != shown) {
            return;  // the viewport shows something else now
        }
        if (!reformatSession_ || reformatSession_->series() != series) {
            reformatSession_ = std::make_shared<MprSession>(volume, series);
            if (AppSettings::instance().quality() == QualityLevel::Performance) {
                reformatSession_->setInterpolation(Interpolation::Nearest);
            }
        }
        grid_->showSource(target, std::make_shared<MprSource>(reformatSession_, o));
        grid_->setActive(target);
        statusBar()->showMessage(tr("Plano %1 reconstruído a partir do volume da série.")
                                     .arg(trCore(toLabel(o))),
                                 5000);
        updateActionStates();
        updateStatusInfo();
    });
}

void MainWindow::cyclePlane() {
    std::optional<MprOrientation> current;
    if (!seriesIn(grid_->activeViewport(), &current) || !current) {
        showPlane(MprOrientation::Sagittal);  // explains why it is not possible
        return;
    }
    // Axial → sagital → coronal → axial.
    switch (*current) {
        case MprOrientation::Axial: showPlane(MprOrientation::Sagittal); break;
        case MprOrientation::Sagittal: showPlane(MprOrientation::Coronal); break;
        case MprOrientation::Coronal: showPlane(MprOrientation::Axial); break;
    }
}

void MainWindow::createMprActions() {
    auto* modeGroup = new QActionGroup(this);
    const std::vector<std::pair<QString, SlabMode>> modes = {{tr("MIP (intensidade máxima)"), SlabMode::MIP},
                                                             {tr("MinIP (intensidade mínima)"), SlabMode::MinIP},
                                                             {tr("Média"), SlabMode::Average}};
    for (const auto& [name, mode] : modes) {
        auto* a = new QAction(name, this);
        a->setCheckable(true);
        a->setChecked(mode == SlabMode::MIP);
        a->setData(static_cast<int>(mode));
        modeGroup->addAction(a);
        slabModeActions_.push_back(a);
        connect(a, &QAction::triggered, this, [this, mode = mode] { setSlab(-1.0, static_cast<int>(mode)); });
    }
    slabGroup_ = new QActionGroup(this);
    slabGroup_->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    for (double t : {0.0, 1.0, 2.0, 3.0, 5.0, 10.0, 20.0, 50.0}) {
        auto* a = new QAction(t == 0.0 ? tr("Plano fino (sem espessura)") : QStringLiteral("%1 mm").arg(t), this);
        a->setCheckable(true);
        a->setChecked(t == 0.0);
        a->setData(t);
        slabGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { setSlab(t, -1); });
    }
    actSlabCustom_ = new QAction(tr("Espessura personalizada…"), this);
    connect(actSlabCustom_, &QAction::triggered, this, [this] {
        bool ok = false;
        const double t = QInputDialog::getDouble(this, tr("Espessura"), tr("Espessura do slab (mm):"), 5.0, 0.1, 500.0,
                                                 1, &ok);
        if (ok) {
            setSlab(t, -1);
        }
    });
    auto rotateAll = [this](double degrees) {
        auto* ms = mprOf(grid_->activeViewport());
        if (ms == nullptr || !grid_->mprSession() || ms->session() != grid_->mprSession()) {
            statusBar()->showMessage(tr("Abra o MPR (%1) e clique no plano em torno do qual os outros devem girar.")
                                         .arg(actMpr_->shortcut().toString(QKeySequence::NativeText)),
                                     6000);
            return;
        }
        ms->session()->rotateOthers(ms->orientation(), degrees);
    };
    // Oblique MPR without the mouse: turn the two other planes around the active view.
    actTurnLeft_ = new QAction(tr("Girar planos −5°"), this);
    actTurnLeft_->setShortcut(QKeySequence(tr("Ctrl+[")));
    connect(actTurnLeft_, &QAction::triggered, this, [rotateAll] { rotateAll(-5.0); });
    actTurnRight_ = new QAction(tr("Girar planos +5°"), this);
    actTurnRight_->setShortcut(QKeySequence(tr("Ctrl+]")));
    connect(actTurnRight_, &QAction::triggered, this, [rotateAll] { rotateAll(5.0); });
    actResetPlanes_ = new QAction(tr("Restaurar planos ortogonais"), this);
    connect(actResetPlanes_, &QAction::triggered, this, [this] {
        if (grid_->mprSession()) {
            grid_->mprSession()->resetOrientation();
        }
    });
    // Shortcuts work from the window even when no menu holds the action yet.
    addAction(actTurnLeft_);
    addAction(actTurnRight_);
}

void MainWindow::populateMprMenu(QMenu* m) {
    auto header = [m](const QString& text) {
        auto* h = m->addAction(text);
        QFont f = h->font();
        f.setBold(true);
        h->setFont(f);
        h->setEnabled(false);
    };
    m->addAction(actMpr_);
    m->addAction(actCrosshairLines_);
    m->addSeparator();
    header(tr("Projeção do slab"));
    for (auto* a : slabModeActions_) {
        m->addAction(a);
    }
    m->addSeparator();
    header(tr("Espessura dos três planos"));
    for (auto* a : slabGroup_->actions()) {
        m->addAction(a);
    }
    m->addAction(actSlabCustom_);
    auto* hint = m->addAction(tr("Para mudar só um plano, arraste a barrinha ao lado da linha dele"));
    hint->setEnabled(false);
    m->addSeparator();
    header(tr("Rotação (MPR oblíquo)"));
    m->addAction(actTurnLeft_);
    m->addAction(actTurnRight_);
    m->addAction(actResetPlanes_);
}

void MainWindow::setSlab(double thickness, int mode) {
    auto session = grid_->mprSession();
    if (!session) {
        // Chosen before opening the MPR: open it and apply the choice.
        pendingSlab_ = std::make_pair(thickness, mode);
        if (!mprBuilding_) {
            startMpr();
        }
        if (!grid_->mprSession() && !mprBuilding_) {
            pendingSlab_.reset();  // nothing to build (no volumetric series)
        }
        return;
    }
    if (mode >= 0) {
        session->setSlabMode(static_cast<SlabMode>(mode));
        const bool allThin = session->slab(MprOrientation::Axial).thickness <= 0.0 &&
                             session->slab(MprOrientation::Coronal).thickness <= 0.0 &&
                             session->slab(MprOrientation::Sagittal).thickness <= 0.0;
        if (allThin && thickness < 0.0) {
            // A projection of a thin plane is the plane itself: start with 10 mm.
            thickness = 10.0;
            statusBar()->showMessage(tr("%1 com 10 mm. Para ajustar, arraste as barras ao lado das linhas coloridas.")
                                         .arg(MprSource::slabModeName(session->slabMode())),
                                     8000);
        }
    }
    if (thickness >= 0.0) {
        session->setSlab({thickness, session->slabMode()});
    }
    syncMprMenu();
}

void MainWindow::syncMprMenu() {
    auto session = grid_->mprSession();
    if (!session) {
        return;
    }
    for (auto* a : slabModeActions_) {
        a->setChecked(a->data().toInt() == static_cast<int>(session->slabMode()));
    }
    const double t = session->slab(MprOrientation::Axial).thickness;
    const bool same = session->slab(MprOrientation::Coronal).thickness == t &&
                      session->slab(MprOrientation::Sagittal).thickness == t;
    for (auto* a : slabGroup_->actions()) {
        a->setChecked(same && std::abs(a->data().toDouble() - t) < 1e-9);
    }
}

void MainWindow::setAccent(Accent a) {
    AppSettings::instance().setAccentColor(Theme::accentKey(a));
    Theme::apply(*qApp, Theme::isDark(), a);
    for (auto* act : accentGroup_->actions()) {
        act->setChecked(act->data().toInt() == static_cast<int>(a));
    }
    // Icons of checked buttons are drawn in the highlight colour: redraw them.
    Icons::clearCache();
    for (auto* act : findChildren<QAction*>()) {
        if (const QString name = act->property("vtcIcon").toString(); !name.isEmpty()) {
            act->setIcon(Icons::get(name));
        }
    }
    for (auto* b : findChildren<QToolButton*>()) {
        if (const QString name = b->property("vtcIcon").toString(); !name.isEmpty()) {
            b->setIcon(Icons::get(name));
        }
    }
    for (auto* w : QApplication::allWidgets()) {
        w->update();
    }
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
                                     .arg(trCore(preset.name))
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
                                                 .arg(trCore(p.name))
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
                                       .arg(trCore(p.name))
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

QPixmap MainWindow::colorMapSwatch(ColorMap map) {
    const auto& table = colorMapTable(map);
    QImage img(64, 12, QImage::Format_RGB32);
    for (int x = 0; x < img.width(); ++x) {
        const QRgb c = table[static_cast<std::size_t>(x * 255 / (img.width() - 1))];
        for (int y = 0; y < img.height(); ++y) {
            img.setPixel(x, y, c);
        }
    }
    return QPixmap::fromImage(img);
}

void MainWindow::applyColorMap(ColorMap map) {
    auto* vp = grid_->activeViewport();
    if (vp == nullptr || !vp->source()) {
        return;
    }
    if (auto* ms = mprOf(vp)) {
        // The three MPR planes show the same volume: same colours.
        for (auto* v : grid_->visibleViewports()) {
            if (auto* other = mprOf(v); other != nullptr && other->session() == ms->session()) {
                v->setColorMap(map);
            }
        }
    } else {
        vp->setColorMap(map);
    }
    if (vp->shownImageIsColor()) {
        statusBar()->showMessage(tr("A tabela de cores vale para imagens em tons de cinza; esta imagem já é colorida."),
                                 6000);
    }
}

void MainWindow::updateActionStates() {
    auto* vp = grid_->activeViewport();
    const bool has = vp != nullptr && vp->source() != nullptr;
    for (auto* a : {actExport_, actCapture_, actInvert_, actRotCw_, actRotCcw_, actFlipH_, actFlipV_, actFit_,
                    actActual_, actReset_, actCine_, actInfo_}) {
        a->setEnabled(has);
    }
    actDelete_->setEnabled(has && vp->selectedAnnotation() != nullptr);
    lutMenu_->setEnabled(has);
    if (has) {
        for (auto* a : lutGroup_->actions()) {
            if (a->data().toInt() == static_cast<int>(vp->colorMap())) {
                a->setChecked(true);
            }
        }
    }
    actDeleteAll_->setEnabled(annotations_->totalCount() > 0);
    actHistogram_->setEnabled(has && vp->selectedAnnotation() != nullptr && vp->selectedAnnotation()->isRoi());
    actMpr_->setEnabled(has || grid_->mprSession() != nullptr);
    actMpr_->setChecked(grid_->mprSession() != nullptr || mprBuilding_);
    toolActions_[Tool::Crosshair]->setEnabled(grid_->mprSession() != nullptr);
    actCrosshairLines_->setEnabled(grid_->mprSession() != nullptr);  // the lines exist only in the MPR
    // Plane button: shows the plane of the active viewport.
    std::optional<MprOrientation> plane;
    const SeriesPtr shown = seriesIn(vp, &plane);
    const bool inMprLayout = grid_->mprSession() && mprOf(vp) != nullptr &&
                             mprOf(vp)->session() == grid_->mprSession();
    actPlane_->setEnabled(shown != nullptr && shown->geometry.volumetric && !inMprLayout);
    for (auto& [o, a] : planeActions_) {
        a->setEnabled(actPlane_->isEnabled());
        a->setChecked(plane && static_cast<int>(*plane) == o);
    }
    if (plane) {
        actPlane_->setIconText(tr("Plano: %1").arg(trCore(toLabel(*plane))));
    } else {
        actPlane_->setIconText(tr("Plano"));
    }
    const InstanceInfo* inst = has ? vp->source()->instanceAt(vp->sliceIndex()) : nullptr;
    actVoiLut_->setEnabled(inst != nullptr && inst->voiLut.has_value());
    if (fitTimer_ != nullptr) {
        fitTimer_->start();  // the Plano label may have changed width
    }
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
        setAccent(Theme::accentFromKey(s.accentColor()));
        if (languageFromKey(s.language()) != currentLanguage()) {
            const auto answer = QMessageBox::question(
                this, tr("Idioma"),
                tr("O idioma escolhido vale a partir da próxima abertura do VisualTC.\n\nReiniciar agora? Os "
                   "exames abertos serão fechados."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer == QMessageBox::Yes) {
                restartApplication();
                return;
            }
        }
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

void MainWindow::restartApplication() {
#ifdef Q_OS_MACOS
    // The bundle, through Launch Services, as when the user opens it.
    const QString bundle = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../.."));
    const bool started = bundle.endsWith(QStringLiteral(".app")) &&
                         QProcess::startDetached(QStringLiteral("/usr/bin/open"), {QStringLiteral("-n"), bundle});
#else
    bool started = false;
    // AppImage: start the image again, not its temporary mount.
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    started = QProcess::startDetached(appImage.isEmpty() ? QCoreApplication::applicationFilePath() : appImage, {});
#endif
    if (!started) {
        QMessageBox::information(this, tr("Idioma"), tr("Feche e abra o VisualTC para usar o novo idioma."));
        return;
    }
    close();
}

void MainWindow::showShortcuts() {
    QString text = tr(
        "<table cellpadding='3'>"
        "<tr><td><b>W</b></td><td>Window/Level</td><td><b>Z</b></td><td>Zoom</td></tr>"
        "<tr><td><b>P</b></td><td>Pan</td><td><b>S</b></td><td>Rolar cortes</td></tr>"
        "<tr><td><b>M</b></td><td>Régua</td><td><b>A</b></td><td>Ângulo</td></tr>"
        "<tr><td><b>C</b></td><td>Ângulo de Cobb</td><td><b>V</b></td><td>Valor do pixel</td></tr>"
        "<tr><td><b>R</b></td><td>ROI retangular</td><td><b>E</b></td><td>ROI elíptica</td></tr>"
        "<tr><td><b>L</b></td><td>ROI livre</td><td><b>X</b></td><td>Linhas do MPR (cruz)</td></tr>"
        "<tr><td><b>I</b></td><td>Inverter</td><td><b>F</b></td><td>Ajustar à janela</td></tr>"
        "<tr><td><b>Shift+F</b></td><td>Tamanho real 1:1</td><td><b>Ctrl+0</b></td><td>Reset</td></tr>"
        "<tr><td><b>[ ]</b></td><td>Girar 90°</td><td><b>H / Shift+H</b></td><td>Espelhar</td></tr>"
        "<tr><td><b>Espaço</b></td><td>Cine play/pause</td><td><b>Esc</b></td><td>Ferramenta padrão</td></tr>"
        "<tr><td><b>1 … 8</b></td><td>Presets de TC</td><td><b>O</b></td><td>Anotações ON/OFF</td></tr>"
        "<tr><td><b>Y</b></td><td>Sincronizar</td><td><b>Ctrl+L</b></td><td>Linhas de referência</td></tr>"
        "<tr><td><b>Ctrl+1…7</b></td><td>Layouts</td><td><b>Tab</b></td><td>Próximo viewport</td></tr>"
        "<tr><td><b>Ctrl+M</b></td><td>MPR</td><td><b>Ctrl+[ ]</b></td><td>MPR oblíquo ±5°</td></tr>"
        "<tr><td><b>Ctrl+Shift+P</b></td><td>Plano: axial → sagital → coronal</td><td><b>Shift+X</b></td>"
        "<td>Posicionar o cruzamento (MPR)</td></tr>"
        "<tr><td><b>↑ ↓ PgUp PgDn</b></td><td>Rolar cortes</td><td><b>Home/End</b></td><td>Primeiro/último</td></tr>"
        "<tr><td><b>Ctrl+Z</b></td><td>Desfazer</td><td><b>Delete</b></td><td>Excluir medida</td></tr>"
        "<tr><td><b>Ctrl+E</b></td><td>Exportar</td><td><b>Ctrl+Shift+C</b></td><td>Copiar imagem</td></tr>"
        "<tr><td><b>Ctrl+Shift+H</b></td><td>Histograma da ROI</td><td></td><td></td></tr>"
        "</table>"
        "<p>Mouse: roda rola os cortes · Ctrl+roda zoom · botão do meio pan · botão direito zoom · "
        "Shift+arrastar pan · duplo clique maximiza/restaura.</p>"
        "<p>MPR: arraste a <b>linha colorida</b> para mover aquele plano, a <b>bolinha</b> na ponta para girar "
        "(oblíquo), a <b>barrinha</b> ao lado para dar espessura (MIP/MinIP) e o <b>círculo central</b> para "
        "mover o cruzamento.</p>");
#ifdef Q_OS_MACOS
    text.replace(QStringLiteral("Ctrl+"), QStringLiteral("⌘"));  // Qt's Ctrl is the Command key on the Mac
#endif
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
           "<p><b>Criado por: Dr Marcelo Duarte - Brasil</b></p>"
           "<p>Visualizador DICOM desktop para TC, RM e outras modalidades, com funcionamento integralmente "
           "offline. Nenhum dado de paciente é enviado a servidores externos.</p>"
           "<p><b>Uso destinado a estudos e pesquisas.</b> Esta versão não é um dispositivo médico registrado "
           "(ANVISA/FDA/CE); medidas e reconstruções devem ser conferidas antes de qualquer uso diagnóstico.</p>"
           "<p>Qt %2 · GDCM %3</p>"
           "<p>Licenças de terceiros: THIRD_PARTY_LICENSES.md</p>")
            .arg(QStringLiteral(VISUALTC_VERSION), QString::fromLatin1(qVersion()),
                 QString::fromStdString(gdcmVersion())));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == toolbar_ && event->type() == QEvent::Resize) {
        fitTimer_->start();
    }
    if (watched == qApp && event->type() == QEvent::FileOpen) {
        const QString file = static_cast<QFileOpenEvent*>(event)->file();
        if (!file.isEmpty()) {
            fileOpenQueue_ << file;
            fileOpenTimer_->start();
        }
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    AppSettings::instance().saveWindow(saveGeometry(), saveState());
    AppSettings::instance().setSeriesPanel(browserDock_->isCollapsed(), browserDock_->expandedWidth());
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
