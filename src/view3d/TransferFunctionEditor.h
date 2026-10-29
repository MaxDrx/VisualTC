#pragma once

#include <QWidget>
#include <vector>

#include "render3d/RenderVolume.h"
#include "render3d/TransferFunction.h"

namespace vtc {

// Opacity curve over the histogram of the volume, with the colour of each
// point below it. Drag a point to move it, double-click empty space to add
// one, double-click a point to change its colour, right-click to remove.
class TransferFunctionEditor : public QWidget {
    Q_OBJECT
public:
    explicit TransferFunctionEditor(QWidget* parent = nullptr);

    void setVolume(const RenderVolumePtr& volume);
    void setTransferFunction(const TransferFunction& tf);  // no signal
    [[nodiscard]] const TransferFunction& transferFunction() const { return tf_; }
    [[nodiscard]] QSize sizeHint() const override { return {260, 150}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {160, 110}; }

Q_SIGNALS:
    void changed(const vtc::TransferFunction& tf);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] double xOf(double value) const;
    [[nodiscard]] double yOf(double opacity) const;
    [[nodiscard]] double valueAt(double x) const;
    [[nodiscard]] double opacityAt(double y) const;
    [[nodiscard]] int pointAt(const QPointF& pos) const;
    void chooseColor(int index);
    void computeRange();

    RenderVolumePtr volume_;
    TransferFunction tf_;
    double lo_ = -1024.0;
    double hi_ = 3071.0;
    std::vector<double> histogram_;  // normalized log counts per pixel column bin
    int dragging_ = -1;
};

}  // namespace vtc
