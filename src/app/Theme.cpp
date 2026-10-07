#include "app/Theme.h"

#include <QApplication>
#include <QObject>
#include <QPalette>
#include <QStyleFactory>
#include <cmath>

namespace vtc {

namespace {
ThemeColors g_colors;
bool g_dark = true;
Accent g_accent = Accent::Blue;

// Mix of two colours (t = share of b).
QColor mix(const QColor& a, const QColor& b, double t) {
    auto ch = [t](int x, int y) { return static_cast<int>(std::lround(x * (1.0 - t) + y * t)); };
    return {ch(a.red(), b.red()), ch(a.green(), b.green()), ch(a.blue(), b.blue())};
}

ThemeColors darkColors() {
    ThemeColors c;
    c.background = QColor("#151515");
    c.backgroundAlt = QColor("#1B1B1B");
    c.panel = QColor("#242424");
    c.panelBorder = QColor("#323232");
    c.text = QColor("#EAEAEA");
    c.textSecondary = QColor("#AAAAAA");
    c.accent = QColor("#2EB4D6");
    c.accentDim = QColor("#1E5F70");
    c.warning = QColor("#E8A33D");
    c.icon = QColor("#D6D6D6");
    return c;
}

ThemeColors lightColors() {
    ThemeColors c;
    c.background = QColor("#ECECEC");
    c.backgroundAlt = QColor("#F5F5F5");
    c.panel = QColor("#FAFAFA");
    c.panelBorder = QColor("#C9C9C9");
    c.text = QColor("#1E1E1E");
    c.textSecondary = QColor("#5A5A5A");
    c.accent = QColor("#137F9C");
    c.accentDim = QColor("#B9E3EE");
    c.warning = QColor("#B26A00");
    c.icon = QColor("#3A3A3A");
    return c;
}
}  // namespace

const ThemeColors& Theme::colors() { return g_colors; }
bool Theme::isDark() { return g_dark; }
Accent Theme::accent() { return g_accent; }

QString Theme::accentName(Accent a) {
    switch (a) {
        case Accent::Blue: return QObject::tr("Azul");
        case Accent::Sepia: return QObject::tr("Sépia");
        case Accent::Yellow: return QObject::tr("Amarelo");
        case Accent::Gold: return QObject::tr("Dourado");
        case Accent::NeonGreen: return QObject::tr("Verde neon");
        case Accent::Orange: return QObject::tr("Laranja");
    }
    return {};
}

QString Theme::accentKey(Accent a) {
    switch (a) {
        case Accent::Blue: return QStringLiteral("blue");
        case Accent::Sepia: return QStringLiteral("sepia");
        case Accent::Yellow: return QStringLiteral("yellow");
        case Accent::Gold: return QStringLiteral("gold");
        case Accent::NeonGreen: return QStringLiteral("neon");
        case Accent::Orange: return QStringLiteral("orange");
    }
    return QStringLiteral("blue");
}

Accent Theme::accentFromKey(const QString& key) {
    for (Accent a : kAccents) {
        if (accentKey(a) == key) {
            return a;
        }
    }
    return Accent::Blue;
}

QColor Theme::accentColor(Accent a, bool dark) {
    // Bright tones for the dark theme; deeper ones keep the contrast on the
    // light theme.
    switch (a) {
        case Accent::Blue: return dark ? QColor("#2EB4D6") : QColor("#137F9C");
        case Accent::Sepia: return dark ? QColor("#C49A6C") : QColor("#8A5A34");
        case Accent::Yellow: return dark ? QColor("#F2D33A") : QColor("#9C7F00");
        case Accent::Gold: return dark ? QColor("#D4AF37") : QColor("#9A7A16");
        case Accent::NeonGreen: return dark ? QColor("#39FF14") : QColor("#1E9E0A");
        case Accent::Orange: return dark ? QColor("#FF8C1A") : QColor("#C25A00");
    }
    return QColor("#2EB4D6");
}

void Theme::apply(QApplication& app, bool dark, Accent accent) {
    g_dark = dark;
    g_accent = accent;
    g_colors = dark ? darkColors() : lightColors();
    g_colors.accent = accentColor(accent, dark);
    // Dim variant: background of checked buttons and selections, dark enough
    // (or light enough) for the normal text colour on top of it.
    g_colors.accentDim = dark ? mix(g_colors.background, g_colors.accent, 0.40)
                              : mix(QColor(Qt::white), g_colors.accent, 0.28);
    const ThemeColors& c = g_colors;
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, c.background);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.backgroundAlt);
    p.setColor(QPalette::AlternateBase, c.panel);
    p.setColor(QPalette::ToolTipBase, c.panel);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::PlaceholderText, c.textSecondary);
    p.setColor(QPalette::Button, c.panel);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, dark ? QColor("#0B0B0B") : QColor("#FFFFFF"));
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Mid, c.panelBorder);
    p.setColor(QPalette::Dark, c.background.darker(130));
    p.setColor(QPalette::Light, c.panel.lighter(130));
    p.setColor(QPalette::Disabled, QPalette::Text, c.textSecondary.darker(140));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textSecondary.darker(140));
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textSecondary.darker(140));
    app.setPalette(p);

    const QString css = QString(R"(
        QMainWindow, QDialog { background: %1; }
        QToolBar { background: %3; border: none; border-bottom: 1px solid %4; spacing: 4px; padding: 3px 6px; }
        QToolBar QToolButton { color: %5; border: 1px solid transparent; border-radius: 4px; padding: 3px 6px; }
        /* Buttons with a menu (Window/Level, Medidas, ROI, Layout, LUT): room for the arrow, so that it never
           covers the label. popupMode 1 = MenuButtonPopup (separate arrow), 2 = InstantPopup. */
        QToolBar QToolButton[popupMode="1"] { padding-right: 20px; }
        QToolBar QToolButton::menu-button { width: 16px; border: none; border-left: 1px solid %4;
                                            border-top-right-radius: 4px; border-bottom-right-radius: 4px; }
        QToolBar QToolButton::menu-button:hover { background: %4; }
        QToolBar QToolButton::menu-arrow { image: url(:/icons/menu-arrow.svg); width: 10px; height: 10px; }
        QToolBar QToolButton[popupMode="2"] { padding-right: 16px; }
        QToolBar QToolButton::menu-indicator { image: url(:/icons/menu-arrow.svg); width: 10px; height: 10px;
                                               subcontrol-origin: padding; subcontrol-position: center right; right: 3px; }
        QToolBar QToolButton:hover { background: %4; }
        QToolBar QToolButton:checked { background: %8; border-color: %7; }
        QToolBar QToolButton:checked:disabled { background: transparent; border-color: transparent; }
        QToolBar QToolButton:pressed { background: %8; }
        QToolBar::separator { background: %4; width: 1px; margin: 4px 6px; }
        QDockWidget { color: %5; titlebar-close-icon: none; }
        QDockWidget::title { background: %3; padding: 6px 8px; border-bottom: 1px solid %4; text-align: left; }
        QTreeWidget, QListWidget { background: %2; border: none; outline: 0; }
        QTreeWidget::item { padding: 4px 2px; border-radius: 4px; }
        QTreeWidget::item:selected { background: %8; color: %5; }
        QTreeWidget::item:hover { background: %3; }
        QHeaderView::section { background: %3; color: %6; border: none; padding: 4px; }
        QStatusBar { background: %3; color: %6; border-top: 1px solid %4; }
        QStatusBar QLabel { color: %6; padding: 0 6px; }
        QMenu { background: %3; color: %5; border: 1px solid %4; padding: 4px; }
        QMenu::item { padding: 5px 24px 5px 22px; border-radius: 3px; }
        QMenu::item:selected { background: %8; }
        QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }
        QMenuBar { background: %3; color: %5; }
        QMenuBar::item:selected { background: %8; }
        QProgressBar { background: %2; border: 1px solid %4; border-radius: 3px; height: 10px; text-align: center; color: %5; }
        QProgressBar::chunk { background: %7; border-radius: 2px; }
        QToolTip { background: %3; color: %5; border: 1px solid %4; padding: 4px; }
        QTabWidget::pane { border: 1px solid %4; }
        QTabBar::tab { background: %3; color: %6; padding: 6px 14px; border: 1px solid %4; border-bottom: none; }
        QTabBar::tab:selected { color: %5; background: %2; }
        QGroupBox { border: 1px solid %4; border-radius: 4px; margin-top: 12px; padding-top: 6px; }
        QGroupBox::title { subcontrol-origin: margin; left: 8px; color: %6; }
    )")
                            .arg(c.background.name(), c.backgroundAlt.name(), c.panel.name(), c.panelBorder.name(),
                                 c.text.name(), c.textSecondary.name(), c.accent.name(), c.accentDim.name());
    app.setStyleSheet(css);
}

}  // namespace vtc
