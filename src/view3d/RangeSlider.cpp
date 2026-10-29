#include "view3d/RangeSlider.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

#include "app/Theme.h"

namespace vtc {

namespace {
constexpr double kMargin = 8.0;
constexpr double kMinGap = 0.01;
}  // namespace

RangeSlider::RangeSlider(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void RangeSlider::setRange(double lo, double hi) {
    lo_ = std::clamp(lo, 0.0, 1.0);
    hi_ = std::clamp(hi, lo_, 1.0);
    update();
}

double RangeSlider::toValue(double x) const {
    return std::clamp((x - kMargin) / std::max(1.0, width() - 2.0 * kMargin), 0.0, 1.0);
}

double RangeSlider::toX(double v) const {
    return kMargin + v * (width() - 2.0 * kMargin);
}

void RangeSlider::paintEvent(QPaintEvent* /*e*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& c = Theme::colors();
    const double cy = height() / 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(c.panelBorder);
    p.drawRoundedRect(QRectF(kMargin, cy - 2, width() - 2 * kMargin, 4), 2, 2);
    p.setBrush(isEnabled() ? c.accent : c.textSecondary);
    p.drawRoundedRect(QRectF(toX(lo_), cy - 2, toX(hi_) - toX(lo_), 4), 2, 2);
    for (int h = 0; h < 2; ++h) {
        const double x = toX(h == 0 ? lo_ : hi_);
        p.setBrush(c.text);
        p.setPen(QPen(hasFocus() && focusHandle_ == h ? c.accent : c.panelBorder, 1.5));
        p.drawEllipse(QPointF(x, cy), 6.5, 6.5);
    }
}

void RangeSlider::mousePressEvent(QMouseEvent* e) {
    const double x = e->position().x();
    const double dl = std::abs(x - toX(lo_));
    const double dh = std::abs(x - toX(hi_));
    if (std::min(dl, dh) <= 9.0) {
        handle_ = (dl < dh || (dl == dh && x < toX(lo_))) ? 0 : 1;
    } else if (x > toX(lo_) && x < toX(hi_)) {
        handle_ = 2;  // the band: moves both ends
        grab_ = toValue(x);
    } else {
        handle_ = x < toX(lo_) ? 0 : 1;
    }
    if (handle_ != 2) {
        focusHandle_ = handle_;
    }
    Q_EMIT dragStarted();
    mouseMoveEvent(e);
}

void RangeSlider::mouseMoveEvent(QMouseEvent* e) {
    if (handle_ < 0) {
        return;
    }
    const double v = toValue(e->position().x());
    if (handle_ == 0) {
        lo_ = std::min(v, hi_ - kMinGap);
    } else if (handle_ == 1) {
        hi_ = std::max(v, lo_ + kMinGap);
    } else {
        const double span = hi_ - lo_;
        const double shift = std::clamp(v - grab_, -lo_, 1.0 - hi_);
        lo_ += shift;
        hi_ = lo_ + span;
        grab_ += shift;
    }
    lo_ = std::max(0.0, lo_);
    hi_ = std::min(1.0, hi_);
    update();
    Q_EMIT rangeChanged(lo_, hi_);
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* /*e*/) {
    if (handle_ >= 0) {
        handle_ = -1;
        Q_EMIT dragFinished();
    }
}

void RangeSlider::keyPressEvent(QKeyEvent* e) {
    double step = 0.0;
    switch (e->key()) {
        case Qt::Key_Left:
        case Qt::Key_Down:
            step = -0.01;
            break;
        case Qt::Key_Right:
        case Qt::Key_Up:
            step = 0.01;
            break;
        case Qt::Key_Space:  // the other handle
            focusHandle_ = 1 - focusHandle_;
            update();
            return;
        default:
            QWidget::keyPressEvent(e);
            return;
    }
    if (focusHandle_ == 0) {
        lo_ = std::clamp(lo_ + step, 0.0, hi_ - kMinGap);
    } else {
        hi_ = std::clamp(hi_ + step, lo_ + kMinGap, 1.0);
    }
    update();
    Q_EMIT rangeChanged(lo_, hi_);
}

}  // namespace vtc
