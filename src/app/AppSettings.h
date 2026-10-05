#pragma once

#include <QList>
#include <QString>
#include <QVariant>

#include "imaging/WindowLevel.h"

namespace vtc {

enum class MouseAction { ActiveTool = 0, WindowLevel, Pan, Zoom, Scroll, None };

enum class QualityLevel { Performance = 0, Balanced, Quality };

// Typed access to persistent preferences (QSettings: registry on Windows,
// plist on macOS, ~/.config on Linux). Never stores patient data.
class AppSettings {
public:
    static AppSettings& instance();

    // Interface
    bool darkTheme() const;
    void setDarkTheme(bool on);
    int fontPointSize() const;  // 0 = system default
    void setFontPointSize(int pt);

    // Mouse
    MouseAction leftButton() const;
    MouseAction middleButton() const;
    MouseAction rightButton() const;
    void setButtons(MouseAction left, MouseAction middle, MouseAction right);

    // Performance
    int cacheMegabytes() const;  // 0 = automatic
    void setCacheMegabytes(int mb);
    bool preloadSeries() const;
    void setPreloadSeries(bool on);
    int decodeThreads() const;  // 0 = automatic
    void setDecodeThreads(int n);
    bool smoothInterpolation() const;
    void setSmoothInterpolation(bool on);
    bool useIsolatedDecoder() const;
    void setUseIsolatedDecoder(bool on);
    QualityLevel quality() const;
    void setQuality(QualityLevel q);

    // DICOM / display
    bool overlaysVisible() const;
    void setOverlaysVisible(bool on);
    bool referenceLines() const;
    void setReferenceLines(bool on);
    bool syncZoomPan() const;
    void setSyncZoomPan(bool on);
    bool syncWindow() const;
    void setSyncWindow(bool on);

    QList<WindowPreset> customPresets() const;
    void setCustomPresets(const QList<WindowPreset>& presets);

    QString lastOpenDirectory() const;
    void setLastOpenDirectory(const QString& dir);

    QByteArray windowGeometry() const;
    QByteArray windowState() const;
    void saveWindow(const QByteArray& geometry, const QByteArray& state);

private:
    AppSettings() = default;
    QVariant value(const QString& key, const QVariant& def) const;
    void setValue(const QString& key, const QVariant& v);
};

}  // namespace vtc
