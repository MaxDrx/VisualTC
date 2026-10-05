#include "ui/HistogramDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>

#include "app/Theme.h"

namespace vtc {

namespace {

class HistogramWidget : public QWidget {
public:
    HistogramWidget(Histogram h, QString unit, QWidget* parent) : QWidget(parent), h_(std::move(h)), unit_(std::move(unit)) {
        setMinimumSize(520, 280);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& c = Theme::colors();
        p.fillRect(rect(), c.backgroundAlt);
        const QRectF plot = QRectF(rect()).adjusted(56, 14, -14, -40);
        p.setPen(c.panelBorder);
        p.drawRect(plot);
        if (h_.counts.empty()) {
            p.setPen(c.textSecondary);
            p.drawText(rect(), Qt::AlignCenter, tr("Sem valores na ROI"));
            return;
        }
        const double maxCount = static_cast<double>(std::max<std::size_t>(1, h_.maxCount()));
        const double bw = plot.width() / static_cast<double>(h_.counts.size());
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        for (std::size_t i = 0; i < h_.counts.size(); ++i) {
            const double hgt = plot.height() * static_cast<double>(h_.counts[i]) / maxCount;
            p.drawRect(QRectF(plot.left() + i * bw, plot.bottom() - hgt, std::max(1.0, bw - 1.0), hgt));
        }
        const QLocale loc;
        p.setPen(c.textSecondary);
        const double maxV = h_.min + h_.binWidth * static_cast<double>(h_.counts.size());
        for (int k = 0; k <= 4; ++k) {
            const double v = h_.min + (maxV - h_.min) * k / 4.0;
            const double x = plot.left() + plot.width() * k / 4.0;
            p.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + 4));
            p.drawText(QRectF(x - 40, plot.bottom() + 6, 80, 16), Qt::AlignCenter, loc.toString(v, 'f', 0));
        }
        p.drawText(QRectF(plot.left(), plot.bottom() + 20, plot.width(), 16), Qt::AlignCenter,
                   unit_.isEmpty() ? tr("Valor") : unit_);
        p.drawText(QRectF(0, plot.top() - 6, 52, 16), Qt::AlignRight | Qt::AlignVCenter,
                   loc.toString(static_cast<qulonglong>(h_.maxCount())));
        p.drawText(QRectF(0, plot.bottom() - 10, 52, 16), Qt::AlignRight | Qt::AlignVCenter, "0");
        p.save();
        p.translate(14, plot.center().y());
        p.rotate(-90);
        p.drawText(QRectF(-60, -8, 120, 16), Qt::AlignCenter, tr("pixels"));
        p.restore();
    }

private:
    Histogram h_;
    QString unit_;
};

}  // namespace

HistogramDialog::HistogramDialog(const std::vector<double>& values, const QString& unit, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Histograma da ROI"));
    auto* layout = new QVBoxLayout(this);
    const int bins = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(values.size()))), 10, 120);
    layout->addWidget(new HistogramWidget(makeHistogram(values, bins), unit, this));
    double mean = 0.0;
    for (double v : values) {
        mean += v;
    }
    mean = values.empty() ? 0.0 : mean / static_cast<double>(values.size());
    const QLocale loc;
    auto* info = new QLabel(tr("%1 pixels  ·  média %2 %3").arg(loc.toString(static_cast<qulonglong>(values.size())),
                                                                 loc.toString(mean, 'f', 1), unit),
                            this);
    layout->addWidget(info);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

}  // namespace vtc
