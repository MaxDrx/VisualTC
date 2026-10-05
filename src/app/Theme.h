#pragma once

#include <QColor>

class QApplication;

namespace vtc {

// Visual identity of the workstation (section 65): sober dark theme, images
// always on pure black, discreet medical cyan for selection.
struct ThemeColors {
    QColor background;
    QColor backgroundAlt;
    QColor panel;
    QColor panelBorder;
    QColor text;
    QColor textSecondary;
    QColor accent;
    QColor accentDim;
    QColor warning;
    QColor icon;
};

class Theme {
public:
    static void apply(QApplication& app, bool dark);
    static const ThemeColors& colors();
    static bool isDark();

    // Fixed overlay/measurement colors (drawn over the black image area,
    // so they do not depend on the UI theme).
    static QColor overlayText() { return QColor(0xE8, 0xE8, 0xE8); }
    static QColor measurement() { return QColor(0xFF, 0xD2, 0x4A); }
    static QColor measurementSelected() { return QColor(0x4A, 0xD6, 0xFF); }
    static QColor referenceLine() { return QColor(0x3E, 0xC9, 0xE6); }
    static QColor axialColor() { return QColor(0xE8, 0xC0, 0x4A); }
    static QColor coronalColor() { return QColor(0x7B, 0xD8, 0x6A); }
    static QColor sagittalColor() { return QColor(0x5A, 0xA8, 0xFF); }
};

}  // namespace vtc
