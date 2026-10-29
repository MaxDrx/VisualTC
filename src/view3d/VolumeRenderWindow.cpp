#include "view3d/VolumeRenderWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

#include "app/AppSettings.h"
#include "app/I18n.h"
#include "core/Logger.h"
#include "imaging/Orientation.h"
#include "render3d/CpuRayCaster.h"
#include "ui/Icons.h"
#include "view3d/RangeSlider.h"
#include "view3d/TransferFunctionEditor.h"
#include "view3d/VolumeRenderView.h"

namespace vtc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kThumb = 76;

RenderMode modeFromIndex(int i) {
    switch (i) {
        case 1:
            return RenderMode::Surface;
        case 2:
            return RenderMode::Mip;
        case 3:
            return RenderMode::MinIp;
        case 4:
            return RenderMode::Average;
        default:
            return RenderMode::Volume;
    }
}

int indexOfMode(RenderMode m) {
    switch (m) {
        case RenderMode::Volume:
            return 0;
        case RenderMode::Surface:
            return 1;
        case RenderMode::Mip:
            return 2;
        case RenderMode::MinIp:
            return 3;
        case RenderMode::Average:
            return 4;
    }
    return 0;
}

QIcon placeholderIcon() {
    QPixmap pm(kThumb, kThumb);
    pm.fill(QColor(16, 16, 18));
    return QIcon(pm);
}

}  // namespace

VolumeRenderWindow::VolumeRenderWindow(VolumePtr source, const QString& seriesId, const QString& modality,
                                       const QString& title, QWidget* parent)
    : QMainWindow(parent, Qt::Window), source_(std::move(source)), seriesId_(seriesId) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("volumeRenderWindow"));
    setWindowTitle(title.isEmpty() ? tr("Reconstrução 3D") : tr("Reconstrução 3D — %1").arg(title));
    isCt_ = (source_ && source_->isCt()) || modality == QLatin1String("CT");
    isMr_ = !isCt_ && modality == QLatin1String("MR");
    buildUi();
    createActions();
    connect(&buildWatcher_, &QFutureWatcher<RenderVolume::BuildResult>::finished, this,
            &VolumeRenderWindow::baseVolumeBuilt);
    connect(&cutsWatcher_, &QFutureWatcher<RenderVolumePtr>::finished, this, &VolumeRenderWindow::cutsFinished);
    connect(&thumbWatcher_, &QFutureWatcher<std::vector<QImage>>::finished, this, [this] {
        const auto images = thumbWatcher_.result();
        for (std::size_t i = 0; i < images.size() && i < thumbKeys_.size(); ++i) {
            for (int row = 0; row < presetList_->count(); ++row) {
                QListWidgetItem* item = presetList_->item(row);
                if (item->data(Qt::UserRole).toString() == thumbKeys_[i]) {
                    item->setIcon(QIcon(QPixmap::fromImage(images[i])));
                }
            }
        }
    });
    const QByteArray geometry = AppSettings::instance().render3dWindowGeometry();
    if (geometry.isEmpty() || !restoreGeometry(geometry)) {
        resize(1240, 820);
    }
    view_->prepareRenderer();  // the resolution of the volume depends on it
    updateStatus();
    buildBaseVolume();
}

VolumeRenderWindow::~VolumeRenderWindow() {
    if (buildCancel_) {
        buildCancel_->store(true);
    }
    buildWatcher_.waitForFinished();
    cutsWatcher_.waitForFinished();
    thumbWatcher_.waitForFinished();
}

bool VolumeRenderWindow::busy() const {
    return buildWatcher_.isRunning() || cutsWatcher_.isRunning() || cutsDirty_;
}

void VolumeRenderWindow::closeEvent(QCloseEvent* e) {
    AppSettings::instance().setRender3dWindowGeometry(saveGeometry());
    view_->setSpinning(false);
    QMainWindow::closeEvent(e);
}

QString VolumeRenderWindow::group() const {
    if (isCt_) {
        return QStringLiteral("ct");
    }
    return isMr_ ? QStringLiteral("mr") : QStringLiteral("other");
}

QString VolumeRenderWindow::presetName(const QString& key) const {
    if (key == QLatin1String("ct-bone"))
        return tr("Ossos");
    if (key == QLatin1String("ct-angio"))
        return tr("Vasos (angio)");
    if (key == QLatin1String("ct-soft"))
        return tr("Partes moles e ossos");
    if (key == QLatin1String("ct-muscle"))
        return tr("Músculos");
    if (key == QLatin1String("ct-skin"))
        return tr("Pele");
    if (key == QLatin1String("ct-lung"))
        return tr("Pulmões");
    if (key == QLatin1String("ct-airway"))
        return tr("Vias aéreas (MinIP)");
    if (key == QLatin1String("ct-mip"))
        return tr("MIP");
    if (key == QLatin1String("ct-xray"))
        return tr("Raio-X simulado");
    if (key == QLatin1String("ct-bone-surface"))
        return tr("Superfície óssea");
    if (key == QLatin1String("mr-default"))
        return tr("Tecidos (RM)");
    if (key == QLatin1String("mr-angio"))
        return tr("Vasos (angio RM)");
    if (key == QLatin1String("mr-mip"))
        return tr("MIP");
    if (key == QLatin1String("generic"))
        return tr("Padrão");
    if (key == QLatin1String("generic-mip"))
        return tr("MIP");
    return key;
}

void VolumeRenderWindow::buildUi() {
    view_ = new VolumeRenderView(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(view_);
    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(buildPanel());
    scroll->setMinimumWidth(300);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({900, 330});
    setCentralWidget(splitter);

    statusLabel_ = new QLabel(this);
    rendererLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addPermanentWidget(rendererLabel_);

    connect(view_, &VolumeRenderView::cutDrawn, this, &VolumeRenderWindow::addCut);
    connect(view_, &VolumeRenderView::transferFunctionDragged, this,
            [this](const TransferFunction& tf) { tfEditor_->setTransferFunction(tf); });
    connect(view_, &VolumeRenderView::settingsDragged, this, [this](const RenderSettings&) { syncSettingsWidgets(); });
    connect(view_, &VolumeRenderView::rendererChanged, this, &VolumeRenderWindow::updateStatus);
    connect(view_, &VolumeRenderView::frameDrawn, this, &VolumeRenderWindow::updateStatus);
}

QWidget* VolumeRenderWindow::buildPanel() {
    auto* panel = new QWidget(this);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(8);

    // Presets with previews.
    auto* presetsBox = new QGroupBox(tr("Predefinições"), panel);
    auto* presetsLayout = new QVBoxLayout(presetsBox);
    presetList_ = new QListWidget(presetsBox);
    presetList_->setViewMode(QListView::IconMode);
    presetList_->setIconSize(QSize(kThumb, kThumb));
    presetList_->setGridSize(QSize(kThumb + 18, kThumb + 34));
    presetList_->setMovement(QListView::Static);
    presetList_->setResizeMode(QListView::Adjust);
    presetList_->setWrapping(true);
    presetList_->setWordWrap(true);
    presetList_->setUniformItemSizes(false);  // names in two lines where needed
    presetList_->setSelectionMode(QAbstractItemView::SingleSelection);
    presetList_->setMinimumHeight(2 * (kThumb + 34) + 8);
    presetList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(presetList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item != nullptr && !updating_) {
            applyPreset(item->data(Qt::UserRole).toString());
        }
    });
    connect(presetList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = presetList_->itemAt(pos);
        if (item == nullptr || !item->data(Qt::UserRole).toString().startsWith(QLatin1String("user:"))) {
            return;
        }
        QMenu menu(this);
        QAction* del = menu.addAction(tr("Excluir predefinição"));
        if (menu.exec(presetList_->viewport()->mapToGlobal(pos)) == del) {
            deleteUserPreset(item->data(Qt::UserRole).toString().mid(5).toInt());
        }
    });
    presetsLayout->addWidget(presetList_);
    auto* savePreset = new QPushButton(tr("Salvar como predefinição…"), presetsBox);
    connect(savePreset, &QPushButton::clicked, this, &VolumeRenderWindow::saveUserPreset);
    presetsLayout->addWidget(savePreset);
    layout->addWidget(presetsBox);

    // Appearance.
    auto* lookBox = new QGroupBox(tr("Aparência"), panel);
    auto* look = new QVBoxLayout(lookBox);
    auto* modeRow = new QFormLayout();
    modeCombo_ = new QComboBox(lookBox);
    modeCombo_->addItems({tr("Volume (VR)"), tr("Superfície"), tr("MIP — intensidade máxima"),
                          tr("MinIP — intensidade mínima"), tr("Média — raio-X simulado")});
    connect(modeCombo_, &QComboBox::currentIndexChanged, this, &VolumeRenderWindow::modeChanged);
    modeRow->addRow(tr("Modo:"), modeCombo_);
    look->addLayout(modeRow);

    modeStack_ = new QStackedWidget(lookBox);
    // Volume: colours and opacity.
    auto* vrPage = new QWidget(modeStack_);
    auto* vr = new QVBoxLayout(vrPage);
    vr->setContentsMargins(0, 0, 0, 0);
    tfEditor_ = new TransferFunctionEditor(vrPage);
    connect(tfEditor_, &TransferFunctionEditor::changed, this, [this](const TransferFunction& tf) {
        presetList_->clearSelection();
        view_->setTransferFunction(tf);
    });
    vr->addWidget(tfEditor_);
    auto* opacityRow = new QHBoxLayout();
    opacityRow->addWidget(new QLabel(tr("Opacidade:"), vrPage));
    opacitySlider_ = new QSlider(Qt::Horizontal, vrPage);
    opacitySlider_->setRange(5, 300);
    opacitySlider_->setValue(100);
    opacityValue_ = new QLabel(QStringLiteral("100%"), vrPage);
    opacityValue_->setMinimumWidth(40);
    connect(opacitySlider_, &QSlider::valueChanged, this, [this](int v) {
        opacityValue_->setText(QStringLiteral("%1%").arg(v));
        if (!updating_) {
            view_->setOpacityScale(static_cast<float>(v) / 100.0F);
        }
    });
    opacityRow->addWidget(opacitySlider_, 1);
    opacityRow->addWidget(opacityValue_);
    vr->addLayout(opacityRow);
    auto* hint = new QLabel(tr("Ctrl + arrastar na imagem desloca as cores (como janela/nível)."), vrPage);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    vr->addWidget(hint);
    modeStack_->addWidget(vrPage);
    // Surface: threshold and colour.
    auto* surfPage = new QWidget(modeStack_);
    auto* surf = new QFormLayout(surfPage);
    surf->setContentsMargins(0, 0, 0, 0);
    surfaceSpin_ = new QDoubleSpinBox(surfPage);
    surfaceSpin_->setRange(-100000.0, 100000.0);
    surfaceSpin_->setDecimals(isCt_ ? 0 : 1);
    surfaceSpin_->setSingleStep(isCt_ ? 10.0 : 1.0);
    connect(surfaceSpin_, &QDoubleSpinBox::valueChanged, this, [this](double) { pushSettings(); });
    surf->addRow(tr("Limiar:"), surfaceSpin_);
    surfaceColorButton_ = new QPushButton(tr("Escolher…"), surfPage);
    connect(surfaceColorButton_, &QPushButton::clicked, this, [this] {
        const auto c = view_->settings().surfaceColor;
        const QColor chosen = QColorDialog::getColor(QColor::fromRgbF(c[0], c[1], c[2]), this, tr("Cor da superfície"));
        if (chosen.isValid()) {
            RenderSettings s = view_->settings();
            s.surfaceColor = {static_cast<float>(chosen.redF()), static_cast<float>(chosen.greenF()),
                              static_cast<float>(chosen.blueF())};
            view_->setSettings(s);
            syncSettingsWidgets();
        }
    });
    surf->addRow(tr("Cor:"), surfaceColorButton_);
    modeStack_->addWidget(surfPage);
    // Projections: window.
    auto* projPage = new QWidget(modeStack_);
    auto* proj = new QFormLayout(projPage);
    proj->setContentsMargins(0, 0, 0, 0);
    windowWidthSpin_ = new QDoubleSpinBox(projPage);
    windowWidthSpin_->setRange(0.001, 200000.0);
    windowCenterSpin_ = new QDoubleSpinBox(projPage);
    windowCenterSpin_->setRange(-100000.0, 100000.0);
    for (QDoubleSpinBox* sp : {windowWidthSpin_, windowCenterSpin_}) {
        sp->setDecimals(isCt_ ? 0 : 1);
        sp->setSingleStep(isCt_ ? 10.0 : 1.0);
        connect(sp, &QDoubleSpinBox::valueChanged, this, [this](double) { pushSettings(); });
    }
    proj->addRow(tr("Janela (L):"), windowWidthSpin_);
    proj->addRow(tr("Nível (C):"), windowCenterSpin_);
    modeStack_->addWidget(projPage);
    look->addWidget(modeStack_);

    shadingCheck_ = new QCheckBox(tr("Iluminação (sombreamento)"), lookBox);
    connect(shadingCheck_, &QCheckBox::toggled, this, [this](bool) { pushSettings(); });
    look->addWidget(shadingCheck_);
    perspectiveCheck_ = new QCheckBox(tr("Perspectiva"), lookBox);
    connect(perspectiveCheck_, &QCheckBox::toggled, this, [this](bool on) { view_->setPerspective(on); });
    look->addWidget(perspectiveCheck_);
    auto* bgRow = new QFormLayout();
    backgroundCombo_ = new QComboBox(lookBox);
    backgroundCombo_->addItems({tr("Preto"), tr("Cinza"), tr("Branco")});
    connect(backgroundCombo_, &QComboBox::currentIndexChanged, this, [this](int) { pushSettings(); });
    bgRow->addRow(tr("Fundo:"), backgroundCombo_);
    look->addLayout(bgRow);
    layout->addWidget(lookBox);

    // Crop box.
    auto* cropBox = new QGroupBox(tr("Recorte (caixa)"), panel);
    auto* cropGrid = new QGridLayout(cropBox);
    for (int a = 0; a < 3; ++a) {
        cropLo_[a] = new QLabel(cropBox);
        cropHi_[a] = new QLabel(cropBox);
        crop_[a] = new RangeSlider(cropBox);
        cropLo_[a]->setMinimumWidth(14);
        cropHi_[a]->setMinimumWidth(14);
        cropGrid->addWidget(cropLo_[a], a, 0);
        cropGrid->addWidget(crop_[a], a, 1);
        cropGrid->addWidget(cropHi_[a], a, 2);
        connect(crop_[a], &RangeSlider::rangeChanged, this, [this](double, double) { pushSettings(); });
        connect(crop_[a], &RangeSlider::dragStarted, this, [this] { view_->setCropBoxVisible(true); });
        connect(crop_[a], &RangeSlider::dragFinished, this, [this] { view_->setCropBoxVisible(false); });
    }
    auto* resetCrop = new QPushButton(tr("Restaurar recorte"), cropBox);
    connect(resetCrop, &QPushButton::clicked, this, [this] {
        for (auto* c : crop_) {
            c->setRange(0.0, 1.0);
        }
        pushSettings();
    });
    cropGrid->addWidget(resetCrop, 3, 0, 1, 3);
    layout->addWidget(cropBox);

    // 3D scissors.
    auto* cutBox = new QGroupBox(tr("Tesoura 3D"), panel);
    auto* cut = new QVBoxLayout(cutBox);
    auto* cutHint = new QLabel(tr("Escolha o que remover e contorne a região sobre a imagem; o corte atravessa "
                                  "todo o volume na direção da vista."),
                               cutBox);
    cutHint->setWordWrap(true);
    cut->addWidget(cutHint);
    auto* cutButtons = new QHBoxLayout();
    removeInsideButton_ = new QPushButton(tr("Remover dentro"), cutBox);
    removeOutsideButton_ = new QPushButton(tr("Remover fora"), cutBox);
    auto* cutGroup = new QButtonGroup(cutBox);
    for (QPushButton* b : {removeInsideButton_, removeOutsideButton_}) {
        b->setCheckable(true);
        b->setIcon(Icons::get(QStringLiteral("scissors")));
        cutGroup->addButton(b);
        cutButtons->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, b] {
            view_->setScissorsRemoveInside(b == removeInsideButton_);
            actScissors_->setChecked(true);
            view_->setTool(VolumeRenderView::Tool::Scissors);
        });
    }
    removeInsideButton_->setChecked(true);
    cut->addLayout(cutButtons);
    auto* undoRow = new QHBoxLayout();
    undoCutButton_ = new QPushButton(Icons::get(QStringLiteral("undo")), tr("Desfazer corte"), cutBox);
    restoreButton_ = new QPushButton(tr("Restaurar volume"), cutBox);
    connect(undoCutButton_, &QPushButton::clicked, this, &VolumeRenderWindow::undoCut);
    connect(restoreButton_, &QPushButton::clicked, this, &VolumeRenderWindow::restoreVolume);
    undoRow->addWidget(undoCutButton_);
    undoRow->addWidget(restoreButton_);
    cut->addLayout(undoRow);
    undoCutButton_->setEnabled(false);
    restoreButton_->setEnabled(false);
    layout->addWidget(cutBox);

    // Standard views.
    auto* viewsBox = new QGroupBox(tr("Vistas"), panel);
    auto* views = new QGridLayout(viewsBox);
    const std::pair<ViewDirection, QString> kViews[] = {
        {ViewDirection::Anterior, tr("Anterior")}, {ViewDirection::Posterior, tr("Posterior")},
        {ViewDirection::Right, tr("Direita")},     {ViewDirection::Left, tr("Esquerda")},
        {ViewDirection::Superior, tr("Superior")}, {ViewDirection::Inferior, tr("Inferior")}};
    int i = 0;
    for (const auto& [direction, name] : kViews) {
        auto* b = new QPushButton(name, viewsBox);
        const ViewDirection dir = direction;
        connect(b, &QPushButton::clicked, this, [this, dir] {
            view_->setSpinning(false);
            actSpin_->setChecked(false);
            view_->setView(dir);
        });
        views->addWidget(b, i / 2, i % 2);
        ++i;
    }
    auto* rotRow = new QHBoxLayout();
    for (const auto& [label, deg, tip] : {std::tuple{QStringLiteral("⟲ 90°"), -90.0, tr("Girar 90° para a esquerda")},
                                          std::tuple{QStringLiteral("⟳ 90°"), 90.0, tr("Girar 90° para a direita")},
                                          std::tuple{QStringLiteral("↻ 180°"), 180.0, tr("Girar 180°")}}) {
        auto* b = new QPushButton(label, viewsBox);
        b->setToolTip(tip);
        const double d = deg;
        connect(b, &QPushButton::clicked, this, [this, d] { view_->rotateBy(d, 0.0); });
        rotRow->addWidget(b);
    }
    views->addLayout(rotRow, 3, 0, 1, 2);
    layout->addWidget(viewsBox);

    // Performance.
    auto* perfBox = new QGroupBox(tr("Desempenho"), panel);
    auto* perf = new QFormLayout(perfBox);
    backendCombo_ = new QComboBox(perfBox);
    backendCombo_->addItems({tr("Automático"), tr("Placa de vídeo (GPU)"), tr("Processador (CPU)")});
    backendCombo_->setToolTip(
        tr("Automático usa a placa de vídeo quando ela oferece OpenGL 3.3; em computadores "
           "sem placa adequada a imagem é calculada pelo processador."));
    perf->addRow(tr("Renderização:"), backendCombo_);
    qualityCombo_ = new QComboBox(perfBox);
    qualityCombo_->addItems({tr("Automática"), tr("Leve (computadores antigos)"), tr("Equilibrada"), tr("Máxima")});
    qualityCombo_->setToolTip(
        tr("Resolução do volume e finura da amostragem. A opção Leve mantém a rotação "
           "fluida em computadores antigos."));
    perf->addRow(tr("Qualidade:"), qualityCombo_);
    backendCombo_->setCurrentIndex(static_cast<int>(view_->backend()));
    qualityCombo_->setCurrentIndex(static_cast<int>(view_->quality()));
    connect(backendCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const std::uint64_t before = VolumeRenderView::qualityParams(view_->effectiveQuality()).volumeVoxels;
        view_->setBackend(static_cast<VolumeRenderView::Backend>(index));
        if (VolumeRenderView::qualityParams(view_->effectiveQuality()).volumeVoxels != before) {
            buildBaseVolume();
        }
        updateStatus();
    });
    connect(qualityCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const std::uint64_t before = VolumeRenderView::qualityParams(view_->effectiveQuality()).volumeVoxels;
        view_->setQuality(static_cast<VolumeRenderView::Quality>(index));
        if (VolumeRenderView::qualityParams(view_->effectiveQuality()).volumeVoxels != before) {
            buildBaseVolume();
        } else if (current_) {
            view_->setVolume(current_, true);
        }
        updateStatus();
    });
    layout->addWidget(perfBox);
    layout->addStretch(1);
    return panel;
}

void VolumeRenderWindow::createActions() {
    auto* tb = addToolBar(tr("Ferramentas 3D"));
    tb->setObjectName(QStringLiteral("toolbar3d"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    toolGroup_ = new QActionGroup(this);
    auto tool = [this, tb](const QString& icon, const QString& text, const QString& tip, VolumeRenderView::Tool t) {
        auto* a = tb->addAction(Icons::get(icon), text);
        a->setToolTip(tip);
        a->setCheckable(true);
        toolGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { view_->setTool(t); });
        return a;
    };
    actRotate_ = tool(QStringLiteral("rotate3d"), tr("Girar"), tr("Girar o volume com o botão esquerdo"),
                      VolumeRenderView::Tool::Rotate);
    actPan_ = tool(QStringLiteral("pan"), tr("Mover"), tr("Mover a imagem (também botão do meio ou Shift)"),
                   VolumeRenderView::Tool::Pan);
    actZoom_ = tool(QStringLiteral("zoom"), tr("Zoom"), tr("Aproximar ou afastar (também botão direito e roda)"),
                    VolumeRenderView::Tool::Zoom);
    actWindow_ =
        tool(QStringLiteral("window-level"), tr("Janela"),
             tr("Ajustar as cores e a opacidade arrastando (também Ctrl + arrastar)"), VolumeRenderView::Tool::Window);
    actScissors_ = tool(QStringLiteral("scissors"), tr("Tesoura"), tr("Contornar uma região para removê-la do volume"),
                        VolumeRenderView::Tool::Scissors);
    actRotate_->setChecked(true);
    view_->setTool(VolumeRenderView::Tool::Rotate);
    tb->addSeparator();
    actResetView_ = tb->addAction(Icons::get(QStringLiteral("reset")), tr("Restaurar vista"));
    actResetView_->setToolTip(tr("Vista anterior, centralizada (Home)"));
    connect(actResetView_, &QAction::triggered, this, [this] {
        actSpin_->setChecked(false);
        view_->resetView();
    });
    actSpin_ = tb->addAction(Icons::get(QStringLiteral("spin")), tr("Girar 360°"));
    actSpin_->setCheckable(true);
    actSpin_->setToolTip(tr("Rotação contínua em torno do eixo vertical"));
    connect(actSpin_, &QAction::toggled, this, [this](bool on) { view_->setSpinning(on); });
    tb->addSeparator();
    actSave_ = tb->addAction(Icons::get(QStringLiteral("capture")), tr("Salvar imagem"));
    actSave_->setShortcut(QKeySequence::Save);
    actSave_->setToolTip(tr("Salvar a imagem 3D em PNG ou JPEG (Ctrl+S)"));
    connect(actSave_, &QAction::triggered, this, &VolumeRenderWindow::saveImage);
    actCopy_ = new QAction(tr("Copiar imagem"), this);
    actCopy_->setShortcut(QKeySequence::Copy);
    addAction(actCopy_);
    connect(actCopy_, &QAction::triggered, this, &VolumeRenderWindow::copyImage);
    actExport360_ = tb->addAction(Icons::get(QStringLiteral("export")), tr("Exportar giro"));
    actExport360_->setToolTip(tr("Salvar uma volta completa como sequência de imagens PNG"));
    connect(actExport360_, &QAction::triggered, this, &VolumeRenderWindow::exportRotation);
}

void VolumeRenderWindow::buildBaseVolume() {
    if (!source_) {
        return;
    }
    if (buildCancel_) {
        buildCancel_->store(true);
    }
    buildWatcher_.waitForFinished();
    const std::uint64_t voxels = VolumeRenderView::qualityParams(view_->effectiveQuality()).volumeVoxels;
    buildCancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancel = buildCancel_;
    const VolumePtr src = source_;
    statusLabel_->setText(tr("Preparando o volume 3D…"));
    buildWatcher_.setFuture(
        QtConcurrent::run([src, voxels, cancel] { return RenderVolume::fromImageVolume(*src, voxels, cancel.get()); }));
}

void VolumeRenderWindow::baseVolumeBuilt() {
    const RenderVolume::BuildResult r = buildWatcher_.result();
    if (!r.volume) {
        if (r.error != "Cancelado.") {
            statusLabel_->clear();
            QMessageBox::warning(this, tr("Reconstrução 3D"), trCore(r.error));
        }
        return;
    }
    const bool first = !base_;
    base_ = r.volume;
    tfEditor_->setVolume(base_);
    updateCropLabels();
    if (cuts_.empty()) {
        showVolume(base_, !first);
    } else {
        recomputeCuts();
    }
    if (first) {
        fillPresetList();
        applyPreset(isCt_ ? QStringLiteral("ct-bone")
                          : (isMr_ ? QStringLiteral("mr-default") : QStringLiteral("generic")));
        makeThumbnails();
        Q_EMIT volumeReady();
    }
    updateStatus();
}

void VolumeRenderWindow::showVolume(const RenderVolumePtr& volume, bool keepCamera) {
    current_ = volume;
    view_->setVolume(volume, keepCamera);
    undoCutButton_->setEnabled(!cuts_.empty());
    restoreButton_->setEnabled(!cuts_.empty());
    updateStatus();
}

void VolumeRenderWindow::addCut(const SculptCut& cut) {
    if (!base_) {
        return;
    }
    cuts_.push_back(cut);
    recomputeCuts();
}

void VolumeRenderWindow::undoCut() {
    if (cuts_.empty()) {
        return;
    }
    cuts_.pop_back();
    recomputeCuts();
}

void VolumeRenderWindow::restoreVolume() {
    if (cuts_.empty()) {
        return;
    }
    cuts_.clear();
    recomputeCuts();
}

void VolumeRenderWindow::recomputeCuts() {
    if (!base_) {
        return;
    }
    if (cutsWatcher_.isRunning()) {
        cutsDirty_ = true;
        return;
    }
    cutsDirty_ = false;
    if (cuts_.empty()) {
        showVolume(base_, true);
        Q_EMIT cutsApplied();
        return;
    }
    statusLabel_->setText(tr("Aplicando o corte…"));
    const RenderVolumePtr base = base_;
    const std::vector<SculptCut> cuts = cuts_;
    cutsWatcher_.setFuture(QtConcurrent::run([base, cuts]() -> RenderVolumePtr { return applyCuts(*base, cuts); }));
}

void VolumeRenderWindow::cutsFinished() {
    const RenderVolumePtr result = cutsWatcher_.result();
    if (cutsDirty_) {
        recomputeCuts();
        return;
    }
    if (result) {
        showVolume(result, true);
    }
    Q_EMIT cutsApplied();
}

void VolumeRenderWindow::fillPresetList() {
    updating_ = true;
    presetList_->clear();
    for (const auto& p : renderPresets()) {
        const bool fits = isCt_ ? p.applies == RenderPreset::Applies::Ct
                                : (p.applies == RenderPreset::Applies::Mr || p.applies == RenderPreset::Applies::Any);
        if (!fits || (!isCt_ && !isMr_ && p.applies == RenderPreset::Applies::Mr)) {
            continue;
        }
        auto* item = new QListWidgetItem(placeholderIcon(), presetName(QString::fromStdString(p.key)), presetList_);
        item->setData(Qt::UserRole, QString::fromStdString(p.key));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setSizeHint(QSize(kThumb + 16, kThumb + 32));
    }
    const auto users = loadUserPresets();
    for (std::size_t i = 0; i < users.size(); ++i) {
        if (users[i].group != group()) {
            continue;
        }
        auto* item = new QListWidgetItem(placeholderIcon(), users[i].name, presetList_);
        item->setData(Qt::UserRole, QStringLiteral("user:%1").arg(i));
        item->setToolTip(tr("Predefinição salva por você (botão direito para excluir)"));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setSizeHint(QSize(kThumb + 16, kThumb + 32));
    }
    updating_ = false;
}

void VolumeRenderWindow::makeThumbnails() {
    if (!base_ || thumbWatcher_.isRunning()) {
        return;
    }
    std::vector<RenderPreset> presets;
    std::vector<float> opacities;
    thumbKeys_.clear();
    const auto users = loadUserPresets();
    for (int row = 0; row < presetList_->count(); ++row) {
        const QString key = presetList_->item(row)->data(Qt::UserRole).toString();
        if (key.startsWith(QLatin1String("user:"))) {
            const auto idx = static_cast<std::size_t>(key.mid(5).toInt());
            if (idx < users.size()) {
                presets.push_back(users[idx].preset);
                opacities.push_back(users[idx].opacity);
                thumbKeys_.push_back(key);
            }
        } else if (const RenderPreset* p = findRenderPreset(key.toStdString())) {
            presets.push_back(*p);
            opacities.push_back(1.0F);
            thumbKeys_.push_back(key);
        }
    }
    const RenderVolumePtr base = base_;
    const int size = static_cast<int>(kThumb * devicePixelRatioF());
    thumbWatcher_.setFuture(QtConcurrent::run([base, presets, opacities, size] {
        std::vector<QImage> out;
        const RenderVolumePtr small = RenderVolume::reduced(base, 4096, 1ULL << 20);
        Camera3D cam;
        cam.setView(ViewDirection::Anterior);
        cam.rotate(0.45, 0.2);
        cam.fit(*small, size, size);
        for (std::size_t i = 0; i < presets.size(); ++i) {
            const ResolvedPreset rp = resolvePreset(presets[i], *small);
            RenderSettings s;
            s.mode = presets[i].mode;
            s.shading = presets[i].shading;
            s.windowCenter = rp.windowCenter;
            s.windowWidth = rp.windowWidth;
            s.surfaceValue = rp.surfaceValue;
            s.surfaceColor = presets[i].surfaceColor;
            s.stepVoxels = 0.8;
            const TransferLut lut = buildTransferLut(rp.tf, *small, s.stepMm(small->minSpacing()), opacities[i]);
            QImage img(size, size, QImage::Format_RGB32);
            CpuRayCaster::render(*small, lut, cam, s, size, size, reinterpret_cast<std::uint32_t*>(img.bits()));
            out.push_back(img);
        }
        return out;
    }));
}

void VolumeRenderWindow::applyPreset(const QString& key) {
    if (!base_) {
        return;
    }
    if (key.startsWith(QLatin1String("user:"))) {
        const auto users = loadUserPresets();
        const auto idx = static_cast<std::size_t>(key.mid(5).toInt());
        if (idx >= users.size()) {
            return;
        }
        applyResolved(users[idx].preset, users[idx].opacity);
    } else if (const RenderPreset* p = findRenderPreset(key.toStdString())) {
        applyResolved(*p, 1.0F);
    } else {
        return;
    }
    presetKey_ = key;
    updating_ = true;
    for (int row = 0; row < presetList_->count(); ++row) {
        if (presetList_->item(row)->data(Qt::UserRole).toString() == key) {
            presetList_->setCurrentRow(row);
        }
    }
    updating_ = false;
}

void VolumeRenderWindow::applyResolved(const RenderPreset& preset, float opacity) {
    const ResolvedPreset rp = resolvePreset(preset, *base_);
    RenderSettings s = view_->settings();
    s.mode = preset.mode;
    s.shading = preset.shading;
    s.windowCenter = rp.windowCenter;
    s.windowWidth = rp.windowWidth;
    s.surfaceValue = rp.surfaceValue;
    s.surfaceColor = preset.surfaceColor;
    view_->setTransferFunction(rp.tf);
    tfEditor_->setTransferFunction(rp.tf);
    view_->setOpacityScale(opacity);
    view_->setSettings(s);
    updating_ = true;
    opacitySlider_->setValue(static_cast<int>(std::lround(opacity * 100.0F)));
    updating_ = false;
    syncSettingsWidgets();
}

void VolumeRenderWindow::modeChanged(int index) {
    if (updating_) {
        return;
    }
    RenderSettings s = view_->settings();
    s.mode = modeFromIndex(index);
    // A sensible window the first time a projection is chosen by hand.
    if ((s.mode == RenderMode::Mip || s.mode == RenderMode::MinIp || s.mode == RenderMode::Average) && base_ &&
        s.windowWidth <= 0.0) {
        s.windowCenter = 0.5 * (base_->minValue() + base_->maxValue());
        s.windowWidth = base_->maxValue() - base_->minValue();
    }
    view_->setSettings(s);
    presetList_->clearSelection();
    syncSettingsWidgets();
}

void VolumeRenderWindow::pushSettings() {
    if (updating_) {
        return;
    }
    RenderSettings s = view_->settings();
    s.surfaceValue = surfaceSpin_->value();
    s.windowWidth = windowWidthSpin_->value();
    s.windowCenter = windowCenterSpin_->value();
    s.shading = shadingCheck_->isChecked();
    switch (backgroundCombo_->currentIndex()) {
        case 1:
            s.background = {0.16F, 0.17F, 0.19F};
            break;
        case 2:
            s.background = {1.0F, 1.0F, 1.0F};
            break;
        default:
            s.background = {0.0F, 0.0F, 0.0F};
            break;
    }
    for (int a = 0; a < 3; ++a) {
        s.crop.lo[static_cast<std::size_t>(a)] = crop_[a]->lower();
        s.crop.hi[static_cast<std::size_t>(a)] = crop_[a]->upper();
    }
    view_->setSettings(s);
}

void VolumeRenderWindow::syncSettingsWidgets() {
    updating_ = true;
    const RenderSettings& s = view_->settings();
    modeCombo_->setCurrentIndex(indexOfMode(s.mode));
    surfaceSpin_->setValue(s.surfaceValue);
    windowWidthSpin_->setValue(s.windowWidth);
    windowCenterSpin_->setValue(s.windowCenter);
    shadingCheck_->setChecked(s.shading);
    shadingCheck_->setEnabled(s.mode == RenderMode::Volume || s.mode == RenderMode::Surface);
    const auto& c = s.surfaceColor;
    surfaceColorButton_->setStyleSheet(
        QStringLiteral("background-color: %1;").arg(QColor::fromRgbF(c[0], c[1], c[2]).name()));
    updating_ = false;
    syncModeWidgets();
}

void VolumeRenderWindow::syncModeWidgets() {
    switch (view_->settings().mode) {
        case RenderMode::Volume:
            modeStack_->setCurrentIndex(0);
            break;
        case RenderMode::Surface:
            modeStack_->setCurrentIndex(1);
            break;
        default:
            modeStack_->setCurrentIndex(2);
            break;
    }
}

void VolumeRenderWindow::updateCropLabels() {
    if (!base_) {
        return;
    }
    const Vec3 axes[3] = {base_->axisX(), base_->axisY(), base_->axisZ()};
    for (int a = 0; a < 3; ++a) {
        // The patient side where each end of the axis lies.
        cropLo_[a]->setText(QString::fromStdString(orientationLetters(-axes[a], 0.5)));
        cropHi_[a]->setText(QString::fromStdString(orientationLetters(axes[a], 0.5)));
    }
}

void VolumeRenderWindow::updateStatus() {
    if (base_ && !busy()) {
        const RenderVolume& v = current_ ? *current_ : *base_;
        QString text = tr("Volume %1 × %2 × %3 · voxel %4 × %5 × %6 mm")
                           .arg(v.nx())
                           .arg(v.ny())
                           .arg(v.nz())
                           .arg(QLocale().toString(v.spacingX(), 'f', 2))
                           .arg(QLocale().toString(v.spacingY(), 'f', 2))
                           .arg(QLocale().toString(v.spacingZ(), 'f', 2));
        if (v.reducedFromSource()) {
            text += tr(" (reduzido para fluidez)");
        }
        if (!cuts_.empty()) {
            text += QStringLiteral(" · ") + tr("%n corte(s)", nullptr, static_cast<int>(cuts_.size()));
        }
        statusLabel_->setText(text);
    }
    QString renderer = view_->rendererDescription();
    if (view_->lastFrameMs() > 0.0) {
        renderer += QStringLiteral(" · %1 ms").arg(std::lround(view_->lastFrameMs()));
    }
    rendererLabel_->setText(renderer);
}

void VolumeRenderWindow::saveImage() {
    const QImage img = view_->snapshot();
    if (img.isNull()) {
        return;
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getSaveFileName(this, tr("Salvar imagem 3D"),
                                                      QDir(dir).filePath(QStringLiteral("VisualTC-3D.png")),
                                                      tr("Imagem PNG (*.png);;Imagem JPEG (*.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }
    if (img.save(path, nullptr, path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive) ? -1 : 92)) {
        statusBar()->showMessage(tr("Imagem salva."), 4000);
    } else {
        QMessageBox::warning(this, tr("Salvar imagem 3D"), tr("Não foi possível salvar a imagem neste local."));
    }
}

void VolumeRenderWindow::copyImage() {
    const QImage img = view_->snapshot();
    if (!img.isNull()) {
        QApplication::clipboard()->setImage(img);
        statusBar()->showMessage(tr("Imagem copiada para a área de transferência."), 4000);
    }
}

void VolumeRenderWindow::exportRotation() {
    if (!current_) {
        return;
    }
    bool ok = false;
    const int frames =
        QInputDialog::getInt(this, tr("Exportar giro"), tr("Imagens na volta completa:"), 36, 4, 360, 1, &ok);
    if (!ok) {
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Pasta para as imagens do giro"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    if (dir.isEmpty()) {
        return;
    }
    const bool wasSpinning = view_->spinning();
    view_->setSpinning(false);
    QProgressDialog progress(tr("Exportando o giro…"), tr("Cancelar"), 0, frames, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    const qreal dpr = view_->devicePixelRatioF();
    const int w = static_cast<int>(view_->width() * dpr);
    const int h = static_cast<int>(view_->height() * dpr);
    const Camera3D start = view_->camera();
    int saved = 0;
    for (int i = 0; i < frames && !progress.wasCanceled(); ++i) {
        progress.setValue(i);
        QCoreApplication::processEvents();
        Camera3D cam = start;
        cam.rotate(2.0 * kPi * i / frames, 0.0);
        const QImage img = view_->renderNow(w, h, &cam);
        if (img.isNull() ||
            !img.save(QDir(dir).filePath(QStringLiteral("VisualTC-3D-%1.png").arg(i + 1, 3, 10, QLatin1Char('0'))))) {
            QMessageBox::warning(this, tr("Exportar giro"), tr("Não foi possível salvar as imagens nesta pasta."));
            break;
        }
        ++saved;
    }
    progress.setValue(frames);
    if (wasSpinning) {
        view_->setSpinning(true);
    }
    statusBar()->showMessage(tr("%n imagem(ns) salva(s).", nullptr, saved), 5000);
}

std::vector<VolumeRenderWindow::UserPreset> VolumeRenderWindow::loadUserPresets() const {
    std::vector<UserPreset> out;
    const QJsonDocument doc = QJsonDocument::fromJson(AppSettings::instance().render3dUserPresets());
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        UserPreset u;
        u.name = o.value(QStringLiteral("name")).toString();
        u.group = o.value(QStringLiteral("group")).toString();
        u.opacity = static_cast<float>(o.value(QStringLiteral("opacity")).toDouble(1.0));
        RenderPreset& p = u.preset;
        p.key = "user";
        p.mode = modeFromIndex(o.value(QStringLiteral("mode")).toInt());
        p.shading = o.value(QStringLiteral("shading")).toBool(true);
        p.windowCenter = o.value(QStringLiteral("wc")).toDouble();
        p.windowWidth = std::max(1e-6, o.value(QStringLiteral("ww")).toDouble(1.0));
        p.surfaceValue = o.value(QStringLiteral("surface")).toDouble();
        const QJsonArray sc = o.value(QStringLiteral("surfaceColor")).toArray();
        if (sc.size() == 3) {
            p.surfaceColor = {static_cast<float>(sc[0].toDouble()), static_cast<float>(sc[1].toDouble()),
                              static_cast<float>(sc[2].toDouble())};
        }
        for (const QJsonValue& pv : o.value(QStringLiteral("points")).toArray()) {
            const QJsonArray a = pv.toArray();
            if (a.size() == 5) {
                p.points.push_back({a[0].toDouble(), static_cast<float>(a[1].toDouble()),
                                    static_cast<float>(a[2].toDouble()), static_cast<float>(a[3].toDouble()),
                                    static_cast<float>(a[4].toDouble())});
            }
        }
        if (!u.name.isEmpty() && p.points.size() >= 2) {
            out.push_back(u);
        }
    }
    return out;
}

void VolumeRenderWindow::storeUserPresets(const std::vector<UserPreset>& list) const {
    QJsonArray arr;
    for (const UserPreset& u : list) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), u.name);
        o.insert(QStringLiteral("group"), u.group);
        o.insert(QStringLiteral("opacity"), static_cast<double>(u.opacity));
        o.insert(QStringLiteral("mode"), indexOfMode(u.preset.mode));
        o.insert(QStringLiteral("shading"), u.preset.shading);
        o.insert(QStringLiteral("wc"), u.preset.windowCenter);
        o.insert(QStringLiteral("ww"), u.preset.windowWidth);
        o.insert(QStringLiteral("surface"), u.preset.surfaceValue);
        o.insert(QStringLiteral("surfaceColor"), QJsonArray{static_cast<double>(u.preset.surfaceColor[0]),
                                                            static_cast<double>(u.preset.surfaceColor[1]),
                                                            static_cast<double>(u.preset.surfaceColor[2])});
        QJsonArray pts;
        for (const TfPoint& p : u.preset.points) {
            pts.append(QJsonArray{p.value, static_cast<double>(p.r), static_cast<double>(p.g), static_cast<double>(p.b),
                                  static_cast<double>(p.opacity)});
        }
        o.insert(QStringLiteral("points"), pts);
        arr.append(o);
    }
    AppSettings::instance().setRender3dUserPresets(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void VolumeRenderWindow::saveUserPreset() {
    if (!base_) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Salvar como predefinição"), tr("Nome da predefinição:"),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    UserPreset u;
    u.name = name.left(40);
    u.group = group();
    u.opacity = static_cast<float>(opacitySlider_->value()) / 100.0F;
    const RenderSettings& s = view_->settings();
    u.preset.key = "user";
    u.preset.mode = s.mode;
    u.preset.shading = s.shading;
    u.preset.windowCenter = s.windowCenter;
    u.preset.windowWidth = s.windowWidth;
    u.preset.surfaceValue = s.surfaceValue;
    u.preset.surfaceColor = s.surfaceColor;
    u.preset.points = view_->transferFunction().points;
    auto list = loadUserPresets();
    list.push_back(u);
    storeUserPresets(list);
    fillPresetList();
    presetKey_ = QStringLiteral("user:%1").arg(list.size() - 1);
    makeThumbnails();
    statusBar()->showMessage(tr("Predefinição \"%1\" salva.").arg(u.name), 4000);
}

void VolumeRenderWindow::deleteUserPreset(int index) {
    auto list = loadUserPresets();
    if (index < 0 || index >= static_cast<int>(list.size())) {
        return;
    }
    list.erase(list.begin() + index);
    storeUserPresets(list);
    fillPresetList();
    makeThumbnails();
}

}  // namespace vtc
