#include "ui/SeriesDock.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <functional>

#include "app/Theme.h"
#include "ui/Icons.h"
#include "ui/SeriesBrowser.h"

namespace vtc {

// The thin vertical strip shown while the panel is collapsed. The whole strip
// is a button: one click anywhere brings the series back.
class SeriesRail : public QWidget {
public:
    explicit SeriesRail(QWidget* parent) : QWidget(parent) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setToolTip(QObject::tr("Mostrar o painel de séries (F2)"));
        setAccessibleName(QObject::tr("Mostrar o painel de séries"));
        setMinimumWidth(0);
    }

    std::function<void()> onClick;

    void setCount(int count) {
        count_ = count;
        update();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        const auto& c = Theme::colors();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), hover_ ? c.panel : c.backgroundAlt);
        const int icon = 18;
        p.drawPixmap((width() - icon) / 2, 8, Icons::get("sidebar").pixmap(icon, icon));

        QString text = QObject::tr("Estudos e séries");
        if (count_ > 0) {
            text += QStringLiteral("  ·  %1").arg(count_);
        }
        QFont f = font();
        p.setFont(f);
        const QFontMetrics fm(f);
        const int top = 8 + icon + 10;
        text = fm.elidedText(text, Qt::ElideRight, std::max(0, height() - top - 8));
        // Rotated 90° clockwise: the text reads from top to bottom.
        p.translate(width() / 2.0, top);
        p.rotate(90);
        p.setPen(hover_ ? c.accent : c.text);
        p.drawText(QPointF(0, (fm.ascent() - fm.descent()) / 2.0), text);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && onClick) {
            onClick();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void enterEvent(QEnterEvent* event) override {
        hover_ = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hover_ = false;
        update();
        QWidget::leaveEvent(event);
    }

private:
    int count_ = 0;
    bool hover_ = false;
};

SeriesDock::SeriesDock(QWidget* parent) : QDockWidget(tr("Estudos e séries"), parent) {
    setObjectName(QStringLiteral("seriesDock"));
    // Not closable: collapsing replaces closing, so the series can never be
    // "lost". Movable between the left and right edges of the window.
    setFeatures(QDockWidget::DockWidgetMovable);
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    // Custom title bar: title + collapse button. Mouse presses on the label
    // are ignored, so the panel can still be dragged by its title.
    auto* header = new QWidget(this);
    headerLayout_ = new QHBoxLayout(header);
    headerLayout_->setSpacing(4);
    title_ = new QLabel(header);
    QFont tf = title_->font();
    tf.setBold(true);
    title_->setFont(tf);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toggle_ = new QToolButton(header);
    toggle_->setObjectName(QStringLiteral("seriesDockToggle"));
    toggle_->setAutoRaise(true);
    toggle_->setIconSize(QSize(16, 16));
    toggle_->setFocusPolicy(Qt::NoFocus);
    connect(toggle_, &QToolButton::clicked, this, &SeriesDock::toggleCollapsed);
    headerLayout_->addWidget(title_, 1);
    headerLayout_->addWidget(toggle_);
    setTitleBarWidget(header);

    stack_ = new QStackedWidget(this);
    browser_ = new SeriesBrowser(stack_);
    rail_ = new SeriesRail(stack_);
    rail_->setObjectName(QStringLiteral("seriesRail"));
    rail_->onClick = [this] { setCollapsed(false); };
    stack_->addWidget(browser_);
    stack_->addWidget(rail_);
    setWidget(stack_);
    setMinimumWidth(kMinExpandedWidth);

    connect(this, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        area_ = area;
        updateHeader();
    });
    updateHeader();
}

void SeriesDock::setCollapsed(bool collapsed) {
    const bool changed = collapsed != collapsed_;
    collapsed_ = collapsed;
    applyState();
    if (changed) {
        Q_EMIT collapsedChanged(collapsed_);
    }
}

void SeriesDock::setExpandedWidth(int width) {
    if (width >= kMinExpandedWidth) {
        expandedWidth_ = width;
    }
}

void SeriesDock::setSeriesCount(int count) { rail_->setCount(count); }

void SeriesDock::applyState() {
    // Changing the size limits resizes the panel on the spot (to the minimum
    // width when expanding): that is not the user's choice of width.
    const bool wasTracking = tracking_;
    tracking_ = false;
    if (collapsed_) {
        stack_->setCurrentWidget(rail_);
        setMinimumWidth(kRailWidth);
        setMaximumWidth(kRailWidth);
    } else {
        stack_->setCurrentWidget(browser_);
        setMinimumWidth(kMinExpandedWidth);
        setMaximumWidth(QWIDGETSIZE_MAX);
        auto* window = qobject_cast<QMainWindow*>(parentWidget());
        if (window != nullptr && !isFloating() && window->dockWidgetArea(this) != Qt::NoDockWidgetArea) {
            window->resizeDocks({this}, {expandedWidth_}, Qt::Horizontal);
        }
    }
    tracking_ = wasTracking;
    updateHeader();
}

void SeriesDock::updateHeader() {
    // The arrow points to where the panel goes: towards the window edge to
    // collapse, away from it to expand.
    const bool rightArea = area_ == Qt::RightDockWidgetArea;
    const bool pointLeft = collapsed_ == rightArea;
    toggle_->setIcon(Icons::get(pointLeft ? QStringLiteral("collapse-left") : QStringLiteral("collapse-right")));
    toggle_->setToolTip(collapsed_ ? tr("Mostrar o painel de séries (F2)")
                                   : tr("Recolher o painel de séries para ampliar a área das imagens (F2)"));
    toggle_->setAccessibleName(collapsed_ ? tr("Mostrar o painel de séries") : tr("Recolher o painel de séries"));
    title_->setVisible(!collapsed_);
    if (collapsed_) {
        headerLayout_->setContentsMargins(0, 3, 0, 3);
        headerLayout_->setAlignment(toggle_, Qt::AlignHCenter);
    } else {
        headerLayout_->setContentsMargins(8, 3, 3, 3);
        headerLayout_->setAlignment(toggle_, Qt::Alignment());
        const QFontMetrics fm(title_->font());
        title_->setText(fm.elidedText(tr("Estudos e séries"), Qt::ElideRight, std::max(0, width() - 48)));
    }
}

void SeriesDock::resizeEvent(QResizeEvent* event) {
    QDockWidget::resizeEvent(event);
    if (!collapsed_ && tracking_ && width() >= kMinExpandedWidth) {
        expandedWidth_ = width();
    }
    updateHeader();
}

void SeriesDock::showEvent(QShowEvent* event) {
    QDockWidget::showEvent(event);
    if (!tracking_) {
        // The first layout of the window (restoreState, size hints) is not a
        // choice of the user: apply the remembered width once it is done and
        // only then start following resizes.
        QTimer::singleShot(0, this, [this] {
            applyState();
            tracking_ = true;
        });
    }
}

}  // namespace vtc
