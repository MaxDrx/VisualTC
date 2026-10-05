#include "export/ImageExporter.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QImageWriter>
#include <QLabel>
#include <QMessageBox>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "app/AppSettings.h"
#include "viewer2d/Viewport.h"

namespace vtc {

QStringList ImageExporter::supportedFormats() {
    QStringList out;
    const auto formats = QImageWriter::supportedImageFormats();
    for (const char* f : {"png", "jpg", "tiff"}) {
        if (formats.contains(f)) {
            out << QString::fromLatin1(f);
        }
    }
    return out;
}

bool ImageExporter::save(const QImage& image, const QString& path, QString* error) {
    QImageWriter writer(path);
    if (path.endsWith(".jpg", Qt::CaseInsensitive) || path.endsWith(".jpeg", Qt::CaseInsensitive)) {
        writer.setQuality(95);
    }
    if (!writer.write(image)) {
        if (error != nullptr) {
            *error = writer.errorString();
        }
        return false;
    }
    return true;
}

void ImageExporter::captureToClipboard(Viewport* viewport, bool withAnnotations) {
    if (viewport == nullptr) {
        return;
    }
    QApplication::clipboard()->setImage(viewport->renderImage(withAnnotations, withAnnotations));
}

bool ImageExporter::exportViewport(QWidget* parent, Viewport* viewport) {
    if (viewport == nullptr || !viewport->source()) {
        return false;
    }
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Exportar imagem"));
    auto* layout = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;
    auto* format = new QComboBox(&dlg);
    for (const auto& f : supportedFormats()) {
        format->addItem(f.toUpper(), f);
    }
    auto* annotations = new QCheckBox(QObject::tr("Incluir anotações (textos e medidas)"), &dlg);
    annotations->setChecked(true);
    auto* hidePatient = new QCheckBox(QObject::tr("Ocultar identificação do paciente"), &dlg);
    hidePatient->setChecked(false);
    QObject::connect(annotations, &QCheckBox::toggled, hidePatient, &QWidget::setEnabled);
    form->addRow(QObject::tr("Formato:"), format);
    layout->addLayout(form);
    layout->addWidget(annotations);
    layout->addWidget(hidePatient);
    auto* note = new QLabel(QObject::tr("A imagem exportada é uma cópia da tela. O arquivo DICOM original não é "
                                        "alterado."),
                            &dlg);
    note->setWordWrap(true);
    note->setStyleSheet("color: gray;");
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    const QString ext = format->currentData().toString();
    QString dir = AppSettings::instance().lastOpenDirectory();
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    }
    const QString path = QFileDialog::getSaveFileName(parent, QObject::tr("Salvar imagem"),
                                                      dir + "/VisualTC." + ext,
                                                      ext.toUpper() + " (*." + ext + ")");
    if (path.isEmpty()) {
        return false;
    }
    const QImage img =
        viewport->renderImage(annotations->isChecked(), annotations->isChecked(), hidePatient->isChecked());
    QString error;
    if (!save(img, path, &error)) {
        QMessageBox::warning(parent, QObject::tr("Exportar imagem"),
                             QObject::tr("Não foi possível salvar a imagem: %1").arg(error));
        return false;
    }
    return true;
}

}  // namespace vtc
