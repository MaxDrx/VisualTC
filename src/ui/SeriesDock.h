#pragma once

#include <QDockWidget>

class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QToolButton;

namespace vtc {

class SeriesBrowser;
class SeriesRail;

// Side panel with the studies and the series thumbnails. To give the images
// more room the doctor can drag its edge (down to a thumbnails-only column)
// or collapse it into a thin rail with the « button / F2; clicking the rail
// (or ») brings the panel back at its previous width.
class SeriesDock : public QDockWidget {
    Q_OBJECT
public:
    static constexpr int kRailWidth = 36;
    static constexpr int kMinExpandedWidth = 104;
    static constexpr int kDefaultWidth = 290;

    explicit SeriesDock(QWidget* parent = nullptr);

    [[nodiscard]] SeriesBrowser* browser() const { return browser_; }

    [[nodiscard]] bool isCollapsed() const { return collapsed_; }
    void setCollapsed(bool collapsed);
    void toggleCollapsed() { setCollapsed(!collapsed_); }

    // Width used when expanded; follows the user's dragging of the edge.
    [[nodiscard]] int expandedWidth() const { return expandedWidth_; }
    void setExpandedWidth(int width);

    // Shown on the collapsed rail ("Séries · 7").
    void setSeriesCount(int count);

Q_SIGNALS:
    void collapsedChanged(bool collapsed);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void applyState();
    void updateHeader();

    SeriesBrowser* browser_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    SeriesRail* rail_ = nullptr;
    QHBoxLayout* headerLayout_ = nullptr;
    QLabel* title_ = nullptr;
    QToolButton* toggle_ = nullptr;
    bool collapsed_ = false;
    bool tracking_ = false;  // expandedWidth_ follows resizes once the window is laid out
    int expandedWidth_ = kDefaultWidth;
    Qt::DockWidgetArea area_ = Qt::LeftDockWidgetArea;
};

}  // namespace vtc
