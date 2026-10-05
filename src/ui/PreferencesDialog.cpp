#include "ui/PreferencesDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "app/AppSettings.h"
#include "core/SystemInfo.h"

namespace vtc {

namespace {
QComboBox* mouseCombo(QWidget* parent, MouseAction current) {
    auto* c = new QComboBox(parent);
    c->addItem(QObject::tr("Ferramenta ativa"), static_cast<int>(MouseAction::ActiveTool));
    c->addItem(QObject::tr("Window/Level"), static_cast<int>(MouseAction::WindowLevel));
    c->addItem(QObject::tr("Pan"), static_cast<int>(MouseAction::Pan));
    c->addItem(QObject::tr("Zoom"), static_cast<int>(MouseAction::Zoom));
    c->addItem(QObject::tr("Navegar cortes"), static_cast<int>(MouseAction::Scroll));
    c->setCurrentIndex(c->findData(static_cast<int>(current)));
    return c;
}
}  // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Preferências"));
    resize(560, 460);
    auto& s = AppSettings::instance();
    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    layout->addWidget(tabs);

    // Interface
    auto* ui = new QWidget;
    auto* uiForm = new QFormLayout(ui);
    darkTheme_ = new QCheckBox(tr("Tema escuro (recomendado para leitura de imagens)"));
    darkTheme_->setChecked(s.darkTheme());
    fontSize_ = new QSpinBox;
    fontSize_->setRange(0, 20);
    fontSize_->setSpecialValueText(tr("Padrão do sistema"));
    fontSize_->setValue(s.fontPointSize());
    uiForm->addRow(darkTheme_);
    uiForm->addRow(tr("Tamanho da fonte:"), fontSize_);
    uiForm->addRow(new QLabel(tr("<i>Tema e fonte são aplicados ao reiniciar o VisualTC.</i>")));
    tabs->addTab(ui, tr("Interface"));

    // Mouse
    auto* mouse = new QWidget;
    auto* mouseForm = new QFormLayout(mouse);
    left_ = mouseCombo(mouse, s.leftButton());
    middle_ = mouseCombo(mouse, s.middleButton());
    right_ = mouseCombo(mouse, s.rightButton());
    mouseForm->addRow(tr("Botão esquerdo:"), left_);
    mouseForm->addRow(tr("Botão do meio:"), middle_);
    mouseForm->addRow(tr("Botão direito:"), right_);
    mouseForm->addRow(new QLabel(tr("Roda: navegar cortes · Ctrl+roda: zoom · Shift+arrastar: pan · "
                                    "Ctrl+arrastar: zoom")));
    tabs->addTab(mouse, tr("Mouse"));

    // Performance
    auto* perf = new QWidget;
    auto* perfForm = new QFormLayout(perf);
    cacheMb_ = new QSpinBox;
    cacheMb_->setRange(0, 65536);
    cacheMb_->setSuffix(" MB");
    cacheMb_->setSpecialValueText(tr("Automático (%1 MB)").arg(defaultCacheBudgetBytes() / (1024 * 1024)));
    cacheMb_->setValue(s.cacheMegabytes());
    preload_ = new QCheckBox(tr("Pré-carregar a série inteira em segundo plano"));
    preload_->setChecked(s.preloadSeries());
    threads_ = new QSpinBox;
    threads_->setRange(0, 32);
    threads_->setSpecialValueText(tr("Automático"));
    threads_->setValue(s.decodeThreads());
    smooth_ = new QCheckBox(tr("Interpolação linear na exibição (desligado = vizinho mais próximo)"));
    smooth_->setChecked(s.smoothInterpolation());
    isolated_ = new QCheckBox(tr("Decodificar arquivos em processo isolado (recomendado)"));
    isolated_->setChecked(s.useIsolatedDecoder());
    isolated_->setToolTip(tr("Arquivos corrompidos ou maliciosos não conseguem encerrar o VisualTC."));
    quality_ = new QComboBox;
    quality_->addItem(tr("Desempenho"), static_cast<int>(QualityLevel::Performance));
    quality_->addItem(tr("Equilibrado"), static_cast<int>(QualityLevel::Balanced));
    quality_->addItem(tr("Qualidade"), static_cast<int>(QualityLevel::Quality));
    quality_->setCurrentIndex(quality_->findData(static_cast<int>(s.quality())));
    perfForm->addRow(tr("Cache de imagens:"), cacheMb_);
    perfForm->addRow(tr("Threads de decodificação:"), threads_);
    perfForm->addRow(tr("Qualidade de reconstrução:"), quality_);
    perfForm->addRow(preload_);
    perfForm->addRow(smooth_);
    perfForm->addRow(isolated_);
    tabs->addTab(perf, tr("Desempenho"));

    // DICOM
    auto* dicom = new QWidget;
    auto* dicomLayout = new QVBoxLayout(dicom);
    overlays_ = new QCheckBox(tr("Mostrar textos sobre a imagem (overlay)"));
    overlays_->setChecked(s.overlaysVisible());
    referenceLines_ = new QCheckBox(tr("Mostrar linhas de referência"));
    referenceLines_->setChecked(s.referenceLines());
    syncZoomPan_ = new QCheckBox(tr("Sincronizar também zoom e pan"));
    syncZoomPan_->setChecked(s.syncZoomPan());
    syncWindow_ = new QCheckBox(tr("Sincronizar também window/level (mesma modalidade)"));
    syncWindow_->setChecked(s.syncWindow());
    dicomLayout->addWidget(overlays_);
    dicomLayout->addWidget(referenceLines_);
    dicomLayout->addWidget(syncZoomPan_);
    dicomLayout->addWidget(syncWindow_);
    dicomLayout->addWidget(new QLabel(tr("Presets personalizados de janela:")));
    presets_ = new QTableWidget(0, 3);
    presets_->setHorizontalHeaderLabels({tr("Nome"), tr("Centro (WL)"), tr("Largura (WW)")});
    presets_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (const auto& p : s.customPresets()) {
        const int r = presets_->rowCount();
        presets_->insertRow(r);
        presets_->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(p.name)));
        presets_->setItem(r, 1, new QTableWidgetItem(QString::number(p.center)));
        presets_->setItem(r, 2, new QTableWidgetItem(QString::number(p.width)));
    }
    dicomLayout->addWidget(presets_);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Adicionar"));
    auto* remove = new QPushButton(tr("Remover"));
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    dicomLayout->addLayout(row);
    connect(add, &QPushButton::clicked, this, [this] {
        const int r = presets_->rowCount();
        presets_->insertRow(r);
        presets_->setItem(r, 0, new QTableWidgetItem(tr("Novo preset")));
        presets_->setItem(r, 1, new QTableWidgetItem("40"));
        presets_->setItem(r, 2, new QTableWidgetItem("400"));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (presets_->currentRow() >= 0) {
            presets_->removeRow(presets_->currentRow());
        }
    });
    tabs->addTab(dicom, tr("DICOM"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void PreferencesDialog::accept() {
    auto& s = AppSettings::instance();
    s.setDarkTheme(darkTheme_->isChecked());
    s.setFontPointSize(fontSize_->value());
    s.setButtons(static_cast<MouseAction>(left_->currentData().toInt()),
                 static_cast<MouseAction>(middle_->currentData().toInt()),
                 static_cast<MouseAction>(right_->currentData().toInt()));
    s.setCacheMegabytes(cacheMb_->value());
    s.setPreloadSeries(preload_->isChecked());
    s.setDecodeThreads(threads_->value());
    s.setSmoothInterpolation(smooth_->isChecked());
    s.setUseIsolatedDecoder(isolated_->isChecked());
    s.setQuality(static_cast<QualityLevel>(quality_->currentData().toInt()));
    s.setOverlaysVisible(overlays_->isChecked());
    s.setReferenceLines(referenceLines_->isChecked());
    s.setSyncZoomPan(syncZoomPan_->isChecked());
    s.setSyncWindow(syncWindow_->isChecked());
    QList<WindowPreset> presets;
    for (int r = 0; r < presets_->rowCount(); ++r) {
        WindowPreset p;
        p.name = presets_->item(r, 0) ? presets_->item(r, 0)->text().trimmed().toStdString() : std::string();
        bool okC = false;
        bool okW = false;
        p.center = presets_->item(r, 1) ? presets_->item(r, 1)->text().replace(',', '.').toDouble(&okC) : 0.0;
        p.width = presets_->item(r, 2) ? presets_->item(r, 2)->text().replace(',', '.').toDouble(&okW) : 0.0;
        if (!p.name.empty() && okC && okW && p.width >= 1.0) {
            presets.push_back(p);
        }
    }
    s.setCustomPresets(presets);
    QDialog::accept();
}

}  // namespace vtc
