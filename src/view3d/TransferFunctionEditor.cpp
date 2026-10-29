#include "view3d/TransferFunctionEditor.h"

#include <QColorDialog>
#include <QContextMenuEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "app/Theme.h"

namespace vtc {

namespace {
constexpr int kBins = 128;
constexpr double kColorBar = 12.0;
constexpr double kRadius = 5.0;
}  // namespace

TransferFunctionEditor::TransferFunctionEditor(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setToolTip(
        tr("Curva de opacidade sobre o histograma do volume.\n"
           "Arraste um ponto para movê-lo; clique duplo em área vazia acrescenta um ponto;\n"
           "clique duplo em um ponto muda a cor; botão direito remove."));
}

void TransferFunctionEditor::setVolume(const RenderVolumePtr& volume) {
    volume_ = volume;
    computeRange();
    update();
}

void TransferFunctionEditor::setTransferFunction(const TransferFunction& tf) {
    tf_ = tf;
    update();
}

void TransferFunctionEditor::computeRange() {
    histogram_.assign(kBins, 0.0);
    if (!volume_) {
        return;
    }
    if (volume_->isCt()) {
        lo_ = std::max(volume_->minValue(), -1024.0);
        hi_ = std::min(volume_->maxValue(), 2000.0);
    } else {
        lo_ = volume_->minValue();
        hi_ = std::max(lo_ + 1.0, volume_->percentile(0.999) * 1.05);
    }
    if (hi_ <= lo_) {
        hi_ = lo_ + 1.0;
    }
    const auto& h = volume_->histogram();
    std::vector<double> counts(kBins, 0.0);
    for (std::size_t b = 0; b < h.size(); ++b) {
        const double v = volume_->toValue((static_cast<double>(b) + 0.5) * 65536.0 / static_cast<double>(h.size()));
        const int bin = static_cast<int>((v - lo_) / (hi_ - lo_) * kBins);
        if (bin >= 0 && bin < kBins) {
            counts[static_cast<std::size_t>(bin)] += h[b];
        }
    }
    double top = 0.0;
    for (int i = 0; i < kBins; ++i) {
        histogram_[static_cast<std::size_t>(i)] = std::log1p(counts[static_cast<std::size_t>(i)]);
        top = std::max(top, histogram_[static_cast<std::size_t>(i)]);
    }
    if (top > 0.0) {
        for (auto& v : histogram_) {
            v /= top;
        }
    }
}

QRectF TransferFunctionEditor::plotRect() const {
    return {kRadius + 1, kRadius + 1, width() - 2 * kRadius - 2, height() - 2 * kRadius - kColorBar - 6};
}

double TransferFunctionEditor::xOf(double value) const {
    const QRectF r = plotRect();
    return r.left() + (value - lo_) / (hi_ - lo_) * r.width();
}

double TransferFunctionEditor::yOf(double opacity) const {
    const QRectF r = plotRect();
    return r.bottom() - std::clamp(opacity, 0.0, 1.0) * r.height();
}

double TransferFunctionEditor::valueAt(double x) const {
    const QRectF r = plotRect();
    return lo_ + (x - r.left()) / std::max(1.0, r.width()) * (hi_ - lo_);
}

double TransferFunctionEditor::opacityAt(double y) const {
    const QRectF r = plotRect();
    return std::clamp((r.bottom() - y) / std::max(1.0, r.height()), 0.0, 1.0);
}

int TransferFunctionEditor::pointAt(const QPointF& pos) const {
    int best = -1;
    double bestD = kRadius + 4.0;
    for (int i = 0; i < static_cast<int>(tf_.points.size()); ++i) {
        const auto& p = tf_.points[static_cast<std::size_t>(i)];
        const double x = std::clamp(xOf(p.value), plotRect().left(), plotRect().right());
        const double d = std::hypot(pos.x() - x, pos.y() - yOf(p.opacity));
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void TransferFunctionEditor::paintEvent(QPaintEvent* /*e*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& c = Theme::colors();
    const QRectF r = plotRect();
    p.fillRect(rect(), c.backgroundAlt);
    p.fillRect(r, QColor(12, 12, 14));
    // Histogram (logarithmic).
    if (!histogram_.empty()) {
        QPainterPath hist;
        hist.moveTo(r.bottomLeft());
        for (int i = 0; i < kBins; ++i) {
            const double x0 = r.left() + r.width() * i / kBins;
            const double x1 = r.left() + r.width() * (i + 1) / kBins;
            const double y = r.bottom() - histogram_[static_cast<std::size_t>(i)] * r.height() * 0.95;
            hist.lineTo(x0, y);
            hist.lineTo(x1, y);
        }
        hist.lineTo(r.bottomRight());
        hist.closeSubpath();
        p.fillPath(hist, QColor(110, 110, 120, 110));
    }
    // Colour bar of the function (opacity shown as brightness over black).
    const QRectF bar(r.left(), r.bottom() + 5, r.width(), kColorBar);
    QImage strip(std::max(1, static_cast<int>(bar.width())), 1, QImage::Format_RGB32);
    for (int x = 0; x < strip.width(); ++x) {
        const double v = lo_ + (x + 0.5) / strip.width() * (hi_ - lo_);
        const auto rgba = tf_.evaluate(v);
        const double k = 0.25 + 0.75 * std::min(1.0, static_cast<double>(rgba[3]) * 3.0);
        auto channel = [k](float level) { return static_cast<int>(static_cast<double>(level) * 255.0 * k); };
        strip.setPixel(x, 0, qRgb(channel(rgba[0]), channel(rgba[1]), channel(rgba[2])));
    }
    p.drawImage(bar, strip);
    p.setPen(QPen(c.panelBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    if (tf_.points.empty()) {
        return;
    }
    // Opacity curve.
    QPolygonF curve;
    curve << QPointF(r.left(), yOf(tf_.evaluate(lo_)[3]));
    for (const auto& pt : tf_.points) {
        const double x = xOf(pt.value);
        if (x > r.left() && x < r.right()) {
            curve << QPointF(x, yOf(pt.opacity));
        }
    }
    curve << QPointF(r.right(), yOf(tf_.evaluate(hi_)[3]));
    p.setPen(QPen(c.accent, 1.6));
    p.drawPolyline(curve);
    for (const auto& pt : tf_.points) {
        const double x = xOf(pt.value);
        if (x < r.left() - 0.5 || x > r.right() + 0.5) {
            continue;
        }
        p.setPen(QPen(QColor(235, 235, 235), 1.2));
        p.setBrush(QColor::fromRgbF(pt.r, pt.g, pt.b));
        p.drawEllipse(QPointF(x, yOf(pt.opacity)), kRadius, kRadius);
    }
    // Scale: ends of the value axis.
    QFont f = font();
    f.setPointSizeF(std::max(6.5, f.pointSizeF() - 2.0));
    p.setFont(f);
    p.setPen(c.textSecondary);
    p.drawText(r.adjusted(3, 2, -3, -2), Qt::AlignLeft | Qt::AlignTop, QString::number(std::lround(lo_)));
    p.drawText(r.adjusted(3, 2, -3, -2), Qt::AlignRight | Qt::AlignTop, QString::number(std::lround(hi_)));
}

void TransferFunctionEditor::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = pointAt(e->position());
    }
}

void TransferFunctionEditor::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_ < 0 || dragging_ >= static_cast<int>(tf_.points.size())) {
        return;
    }
    auto& pts = tf_.points;
    const auto i = static_cast<std::size_t>(dragging_);
    // A point stays between its neighbours.
    const double gap = 1e-3 * (hi_ - lo_);
    const double minV = i > 0 ? pts[i - 1].value + gap : lo_ - (hi_ - lo_);
    const double maxV = i + 1 < pts.size() ? pts[i + 1].value - gap : hi_ + 2.0 * (hi_ - lo_);
    if (minV <= maxV) {
        pts[i].value = std::clamp(valueAt(e->position().x()), minV, maxV);
    }
    pts[i].opacity = static_cast<float>(opacityAt(e->position().y()));
    update();
    Q_EMIT changed(tf_);
}

void TransferFunctionEditor::mouseReleaseEvent(QMouseEvent* /*e*/) {
    dragging_ = -1;
}

void TransferFunctionEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    const int hit = pointAt(e->position());
    if (hit >= 0) {
        chooseColor(hit);
        return;
    }
    if (!plotRect().contains(e->position())) {
        return;
    }
    const double v = valueAt(e->position().x());
    const auto rgba = tf_.evaluate(v);
    tf_.points.push_back({v, rgba[0], rgba[1], rgba[2], static_cast<float>(opacityAt(e->position().y()))});
    tf_.sort();
    update();
    Q_EMIT changed(tf_);
}

void TransferFunctionEditor::contextMenuEvent(QContextMenuEvent* e) {
    const int hit = pointAt(e->pos());
    if (hit < 0) {
        return;
    }
    QMenu menu(this);
    QAction* color = menu.addAction(tr("Cor do ponto…"));
    QAction* remove = menu.addAction(tr("Remover ponto"));
    remove->setEnabled(tf_.points.size() > 2);
    QAction* chosen = menu.exec(e->globalPos());
    if (chosen == color) {
        chooseColor(hit);
    } else if (chosen == remove) {
        tf_.points.erase(tf_.points.begin() + hit);
        update();
        Q_EMIT changed(tf_);
    }
}

void TransferFunctionEditor::chooseColor(int index) {
    auto& pt = tf_.points[static_cast<std::size_t>(index)];
    const QColor chosen = QColorDialog::getColor(QColor::fromRgbF(pt.r, pt.g, pt.b), this, tr("Cor do ponto"));
    if (!chosen.isValid()) {
        return;
    }
    pt.r = static_cast<float>(chosen.redF());
    pt.g = static_cast<float>(chosen.greenF());
    pt.b = static_cast<float>(chosen.blueF());
    update();
    Q_EMIT changed(tf_);
}

}  // namespace vtc
