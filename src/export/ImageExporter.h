#pragma once

#include <QImage>
#include <QString>

class QWidget;

namespace vtc {

class Viewport;

// Viewport export (PNG/JPEG/TIFF when available) and clipboard capture.
// Only the viewport area is captured (section 42); exported pixels are a
// rendering of the display, never written back to the DICOM files.
class ImageExporter {
public:
    static bool exportViewport(QWidget* parent, Viewport* viewport);
    static void captureToClipboard(Viewport* viewport, bool withAnnotations);
    static bool save(const QImage& image, const QString& path, QString* error);
    static QStringList supportedFormats();
};

}  // namespace vtc
