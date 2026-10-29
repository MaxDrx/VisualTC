#pragma once

#include <QWidget>

namespace vtc {

// Horizontal slider with two handles (crop box: where each axis begins and
// ends). Values are fractions 0..1.
class RangeSlider : public QWidget {
    Q_OBJECT
public:
    explicit RangeSlider(QWidget* parent = nullptr);

    void setRange(double lo, double hi);  // no signal
    [[nodiscard]] double lower() const { return lo_; }
    [[nodiscard]] double upper() const { return hi_; }
    [[nodiscard]] QSize sizeHint() const override { return {160, 22}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {60, 22}; }

Q_SIGNALS:
    void rangeChanged(double lo, double hi);
    void dragStarted();
    void dragFinished();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    [[nodiscard]] double toValue(double x) const;
    [[nodiscard]] double toX(double v) const;

    double lo_ = 0.0;
    double hi_ = 1.0;
    int handle_ = -1;  // 0 lower, 1 upper, 2 both
    double grab_ = 0.0;
    int focusHandle_ = 0;
};

}  // namespace vtc
