#include "ui/DicomInfoDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLocale>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/I18n.h"
#include "dicom/DicomParser.h"
#include "dicom/TextUtil.h"

namespace vtc {

namespace {
QString s(const std::string& v) { return v.empty() ? QStringLiteral("N/A") : QString::fromStdString(v); }
QString d(std::optional<double> v, int dec = 3) {
    return v ? QLocale().toString(*v, 'f', dec) : QStringLiteral("N/A");
}
QString i(std::optional<int> v) { return v ? QString::number(*v) : QStringLiteral("N/A"); }
QString vec(const Vec3& v) {
    const QLocale l;
    return l.toString(v.x, 'f', 4) + " \\ " + l.toString(v.y, 'f', 4) + " \\ " + l.toString(v.z, 'f', 4);
}
QString spacingName(SpacingSource src) {
    switch (src) {
        case SpacingSource::None: return QObject::tr("ausente (medidas em pixels)");
        case SpacingSource::PixelSpacing: return QStringLiteral("Pixel Spacing (0028,0030)");
        case SpacingSource::ImagerPixelSpacing:
            return QStringLiteral("Imager Pixel Spacing (0018,1164) — ") + QObject::tr("plano do detector");
        case SpacingSource::EnhancedPixelMeasures: return QStringLiteral("Pixel Measures (Enhanced)");
        case SpacingSource::UltrasoundRegion: return QObject::tr("Região de ultrassom calibrada");
    }
    return {};
}
}  // namespace

DicomInfoDialog::DicomInfoDialog(const SeriesPtr& series, int frameIndex, QWidget* parent,
                                 const std::function<std::string(const std::string&)>& displayPath)
    : QDialog(parent) {
    setWindowTitle(tr("Informações DICOM"));
    resize(640, 640);
    auto* layout = new QVBoxLayout(this);
    auto* tree = new QTreeWidget(this);
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Atributo"), tr("Valor")});
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->setAlternatingRowColors(true);
    layout->addWidget(tree);

    auto group = [&](const QString& title) {
        auto* g = new QTreeWidgetItem(tree, {title});
        QFont f = g->font(0);
        f.setBold(true);
        g->setFont(0, f);
        g->setExpanded(true);
        return g;
    };
    auto add = [](QTreeWidgetItem* g, const QString& k, const QString& v) { new QTreeWidgetItem(g, {k, v}); };

    if (!series || series->frames.empty()) {
        return;
    }
    frameIndex = std::clamp(frameIndex, 0, series->frameCount() - 1);
    const FrameRef& ref = series->frames[static_cast<size_t>(frameIndex)];
    const InstanceInfo& inst = *ref.instance;
    const FrameInfo& fi = ref.info();

    auto* gp = group(tr("Paciente e estudo"));
    add(gp, "Patient Name", s(inst.patientName));
    add(gp, "Patient ID", s(inst.patientId));
    add(gp, "Patient Birth Date", s(formatDicomDate(inst.patientBirthDate)));
    add(gp, "Patient Sex / Age", s(inst.patientSex) + " / " + s(inst.patientAge));
    add(gp, "Study Date / Time", s(formatDicomDate(inst.studyDate)) + " " + s(formatDicomTime(inst.studyTime)));
    add(gp, "Study Description", s(inst.studyDescription));
    add(gp, "Accession Number", s(inst.accessionNumber));
    add(gp, "Institution", s(inst.institutionName));
    add(gp, "Study Instance UID", s(inst.studyInstanceUid));

    auto* gs = group(tr("Série"));
    add(gs, "Modality", s(inst.modality));
    add(gs, "Series Number", i(inst.seriesNumber));
    add(gs, "Series Description", s(inst.seriesDescription));
    add(gs, "Body Part", s(inst.bodyPartExamined));
    add(gs, "Manufacturer / Model", s(inst.manufacturer) + " / " + s(inst.manufacturerModel));
    add(gs, "Patient Position", s(inst.patientPosition));
    add(gs, "Series Instance UID", s(inst.seriesInstanceUid));
    add(gs, "Frame of Reference UID", s(inst.frameOfReferenceUid));

    const auto& g = series->geometry;
    auto* gg = group(tr("Geometria da série (análise do VisualTC)"));
    add(gg, tr("Imagens"), QString::number(series->frameCount()));
    add(gg, tr("Reconstrução volumétrica (MPR)"), g.volumetric ? tr("possível") : tr("indisponível"));
    if (g.spatial && g.parallel && g.sliceSpacing > 0.0) {
        add(gg, tr("Espaçamento entre cortes (mediana)"), d(g.sliceSpacing) + " mm");
        add(gg, tr("Espaçamento mín / máx"), d(g.minSpacing) + " / " + d(g.maxSpacing) + " mm");
        add(gg, tr("Espaçamento uniforme"), g.uniformSpacing ? tr("sim") : tr("não"));
        if (g.gapCount > 0) {
            add(gg, tr("Lacunas (cortes ausentes)"), QString::number(g.gapCount));
        }
        if (g.tiltDegrees > 0.0) {
            add(gg, tr("Inclinação do gantry"), d(g.tiltDegrees, 2) + "°");
        }
    }
    for (auto issue : g.issues) {
        add(gg, tr("Aviso"), trCore(describe(issue)));
    }

    auto* gi = group(tr("Imagem %1").arg(frameIndex + 1));
    add(gi, "SOP Class UID", s(inst.sopClassUid));
    add(gi, "SOP Instance UID", s(inst.sopInstanceUid));
    add(gi, "Transfer Syntax", QString::fromStdString(transferSyntaxName(inst.transferSyntaxUid)) + " (" +
                                   s(inst.transferSyntaxUid) + ")");
    add(gi, "Instance Number", i(inst.instanceNumber));
    if (inst.numberOfFrames > 1) {
        add(gi, "Frame", QString::number(ref.frame + 1) + " / " + QString::number(inst.numberOfFrames));
    }
    add(gi, "Rows × Columns", QString::number(inst.rows) + " × " + QString::number(inst.columns));
    add(gi, "Photometric Interpretation", s(inst.photometricInterpretation));
    add(gi, "Bits Allocated / Stored / High Bit",
        QString("%1 / %2 / %3").arg(inst.bitsAllocated).arg(inst.bitsStored).arg(inst.highBit));
    add(gi, "Pixel Representation", inst.pixelRepresentation == 1 ? tr("com sinal") : tr("sem sinal"));
    add(gi, "Rescale Slope / Intercept", d(fi.rescaleSlope, 4) + " / " + d(fi.rescaleIntercept, 4));
    add(gi, "Image Position (Patient)", fi.geometry.hasPosition ? vec(fi.geometry.position) : "N/A");
    add(gi, "Image Orientation (Patient)",
        fi.geometry.hasOrientation ? vec(fi.geometry.rowDir) + "  |  " + vec(fi.geometry.colDir) : "N/A");
    add(gi, tr("Espaçamento de pixel (linhas \\ colunas)"),
        fi.geometry.hasSpacing() ? d(fi.geometry.spacingY, 4) + " \\ " + d(fi.geometry.spacingX, 4) + " mm"
                                 : QStringLiteral("N/A"));
    add(gi, tr("Origem da calibração"), spacingName(fi.geometry.spacingSource));
    add(gi, "Slice Thickness", d(fi.geometry.sliceThickness) + " mm");
    add(gi, "Spacing Between Slices", d(inst.spacingBetweenSlices) + " mm");
    QStringList windows;
    for (const auto& w : fi.windows) {
        windows << QString("%1 / %2").arg(QLocale().toString(w.center, 'f', 0), QLocale().toString(w.width, 'f', 0));
    }
    add(gi, "Window Center / Width", windows.isEmpty() ? "N/A" : windows.join("; "));
    add(gi, "VOI LUT", inst.voiLut ? tr("presente") : tr("ausente"));
    add(gi, tr("Compressão com perdas"), inst.lossyCompressed ? tr("sim") : tr("não"));
    add(gi, tr("Arquivo"), QString::fromStdString(displayPath ? displayPath(inst.filePath) : inst.filePath));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

}  // namespace vtc
