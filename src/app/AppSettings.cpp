#include "app/AppSettings.h"

#include <QSettings>

namespace vtc {

AppSettings& AppSettings::instance() {
    static AppSettings s;
    return s;
}

QVariant AppSettings::value(const QString& key, const QVariant& def) const {
    QSettings s;
    return s.value(key, def);
}

void AppSettings::setValue(const QString& key, const QVariant& v) {
    QSettings s;
    s.setValue(key, v);
}

bool AppSettings::darkTheme() const { return value("ui/darkTheme", true).toBool(); }
void AppSettings::setDarkTheme(bool on) { setValue("ui/darkTheme", on); }
int AppSettings::fontPointSize() const { return value("ui/fontPointSize", 0).toInt(); }
void AppSettings::setFontPointSize(int pt) { setValue("ui/fontPointSize", pt); }

MouseAction AppSettings::leftButton() const {
    return static_cast<MouseAction>(value("mouse/left", static_cast<int>(MouseAction::ActiveTool)).toInt());
}
MouseAction AppSettings::middleButton() const {
    return static_cast<MouseAction>(value("mouse/middle", static_cast<int>(MouseAction::Pan)).toInt());
}
MouseAction AppSettings::rightButton() const {
    return static_cast<MouseAction>(value("mouse/right", static_cast<int>(MouseAction::Zoom)).toInt());
}
void AppSettings::setButtons(MouseAction left, MouseAction middle, MouseAction right) {
    setValue("mouse/left", static_cast<int>(left));
    setValue("mouse/middle", static_cast<int>(middle));
    setValue("mouse/right", static_cast<int>(right));
}

int AppSettings::cacheMegabytes() const { return value("perf/cacheMB", 0).toInt(); }
void AppSettings::setCacheMegabytes(int mb) { setValue("perf/cacheMB", mb); }
bool AppSettings::preloadSeries() const { return value("perf/preload", true).toBool(); }
void AppSettings::setPreloadSeries(bool on) { setValue("perf/preload", on); }
int AppSettings::decodeThreads() const { return value("perf/threads", 0).toInt(); }
void AppSettings::setDecodeThreads(int n) { setValue("perf/threads", n); }
bool AppSettings::smoothInterpolation() const { return value("display/smooth", true).toBool(); }
void AppSettings::setSmoothInterpolation(bool on) { setValue("display/smooth", on); }
bool AppSettings::useIsolatedDecoder() const { return value("perf/isolatedDecoder", true).toBool(); }
void AppSettings::setUseIsolatedDecoder(bool on) { setValue("perf/isolatedDecoder", on); }
QualityLevel AppSettings::quality() const {
    return static_cast<QualityLevel>(value("perf/quality", static_cast<int>(QualityLevel::Balanced)).toInt());
}
void AppSettings::setQuality(QualityLevel q) { setValue("perf/quality", static_cast<int>(q)); }

bool AppSettings::overlaysVisible() const { return value("display/overlays", true).toBool(); }
void AppSettings::setOverlaysVisible(bool on) { setValue("display/overlays", on); }
bool AppSettings::referenceLines() const { return value("display/referenceLines", true).toBool(); }
void AppSettings::setReferenceLines(bool on) { setValue("display/referenceLines", on); }
bool AppSettings::syncZoomPan() const { return value("sync/zoomPan", false).toBool(); }
void AppSettings::setSyncZoomPan(bool on) { setValue("sync/zoomPan", on); }
bool AppSettings::syncWindow() const { return value("sync/window", false).toBool(); }
void AppSettings::setSyncWindow(bool on) { setValue("sync/window", on); }

QList<WindowPreset> AppSettings::customPresets() const {
    QList<WindowPreset> out;
    QSettings s;
    const int n = s.beginReadArray("presets/custom");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        WindowPreset p;
        p.name = s.value("name").toString().toStdString();
        p.center = s.value("center").toDouble();
        p.width = std::max(1.0, s.value("width").toDouble());
        if (!p.name.empty()) {
            out.push_back(p);
        }
    }
    s.endArray();
    return out;
}

void AppSettings::setCustomPresets(const QList<WindowPreset>& presets) {
    QSettings s;
    s.beginWriteArray("presets/custom", static_cast<int>(presets.size()));
    for (int i = 0; i < presets.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", QString::fromStdString(presets[i].name));
        s.setValue("center", presets[i].center);
        s.setValue("width", presets[i].width);
    }
    s.endArray();
}

QString AppSettings::lastOpenDirectory() const { return value("io/lastDir", QString()).toString(); }
void AppSettings::setLastOpenDirectory(const QString& dir) { setValue("io/lastDir", dir); }

QByteArray AppSettings::windowGeometry() const { return value("ui/geometry", QByteArray()).toByteArray(); }
QByteArray AppSettings::windowState() const { return value("ui/state", QByteArray()).toByteArray(); }
void AppSettings::saveWindow(const QByteArray& geometry, const QByteArray& state) {
    setValue("ui/geometry", geometry);
    setValue("ui/state", state);
}

bool AppSettings::seriesPanelCollapsed() const { return value("ui/seriesPanelCollapsed", false).toBool(); }
int AppSettings::seriesPanelWidth() const { return value("ui/seriesPanelWidth", 0).toInt(); }
void AppSettings::setSeriesPanel(bool collapsed, int width) {
    setValue("ui/seriesPanelCollapsed", collapsed);
    setValue("ui/seriesPanelWidth", width);
}

}  // namespace vtc
