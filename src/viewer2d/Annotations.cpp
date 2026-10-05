#include "viewer2d/Annotations.h"

#include <QFontMetricsF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <numbers>

#include "app/Theme.h"
#include "measurements/RoiStatistics.h"

namespace vtc {

namespace {

QPointF toQ(const Point2& p) { return {p.x, p.y}; }

QString num(double v, int decimals) { return QLocale().toString(v, 'f', decimals); }

double segmentDistance(const QPointF& p, const QPointF& a, const QPointF& b) {
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = len2 > 0.0 ? ((p - a).x() * ab.x() + (p - a).y() * ab.y()) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF proj = a + ab * t;
    return std::hypot(p.x() - proj.x(), p.y() - proj.y());
}

// Outline of the annotation in IMAGE coordinates (closed for ROIs).
std::vector<Point2> outline(AnnotationKind kind, const std::vector<Point2>& pts) {
    std::vector<Point2> out;
    if (pts.size() < 2) {
        return pts;
    }
    switch (kind) {
        case AnnotationKind::Rectangle: {
            const Point2& a = pts[0];
            const Point2& b = pts[1];
            out = {{a.x, a.y}, {b.x, a.y}, {b.x, b.y}, {a.x, b.y}, {a.x, a.y}};
            break;
        }
        case AnnotationKind::Ellipse: {
            const double cx = (pts[0].x + pts[1].x) / 2.0;
            const double cy = (pts[0].y + pts[1].y) / 2.0;
            const double rx = std::abs(pts[1].x - pts[0].x) / 2.0;
            const double ry = std::abs(pts[1].y - pts[0].y) / 2.0;
            for (int i = 0; i <= 72; ++i) {
                const double t = 2.0 * std::numbers::pi * i / 72.0;
                out.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
            }
            break;
        }
        case AnnotationKind::Freehand:
            out = pts;
            out.push_back(pts.front());
            break;
        default:
            out = pts;
    }
    return out;
}

}  // namespace

QString formatLength(double mm, bool calibrated, SpacingSource source) {
    if (!calibrated) {
        return num(mm, 1) + " px";
    }
    QString s = mm >= 100.0 ? num(mm / 10.0, 2) + " cm" : num(mm, 1) + " mm";
    if (source == SpacingSource::ImagerPixelSpacing) {
        s += " (detector)";
    }
    return s;
}

int Annotation::requiredPoints() const {
    switch (kind_) {
        case AnnotationKind::Distance: return 2;
        case AnnotationKind::Angle: return 3;
        case AnnotationKind::Cobb: return 4;
        case AnnotationKind::Rectangle:
        case AnnotationKind::Ellipse: return 2;
        case AnnotationKind::Freehand: return 0;
    }
    return 2;
}

bool Annotation::isComplete() const {
    if (kind_ == AnnotationKind::Freehand) {
        return finished && points.size() >= 3;
    }
    return static_cast<int>(points.size()) >= requiredPoints();
}

Point2 Annotation::labelAnchor() const {
    if (points.empty()) {
        return {};
    }
    switch (kind_) {
        case AnnotationKind::Distance: return points.back();
        case AnnotationKind::Angle: return points.size() >= 2 ? points[1] : points[0];
        case AnnotationKind::Cobb:
            if (points.size() >= 4) {
                return {(points[2].x + points[3].x) / 2.0, (points[2].y + points[3].y) / 2.0};
            }
            return points.back();
        case AnnotationKind::Rectangle:
        case AnnotationKind::Ellipse:
            if (points.size() >= 2) {
                return {std::max(points[0].x, points[1].x), std::min(points[0].y, points[1].y)};
            }
            return points[0];
        case AnnotationKind::Freehand: {
            Point2 best = points[0];
            for (const auto& p : points) {
                if (p.x > best.x) {
                    best = p;
                }
            }
            return best;
        }
    }
    return points[0];
}

std::vector<double> Annotation::roiValues(const MeasureContext& ctx) const {
    std::vector<double> values;
    if (ctx.frame == nullptr || !isRoi() || points.size() < 2) {
        return values;
    }
    switch (kind_) {
        case AnnotationKind::Rectangle: rectangleStats(*ctx.frame, points[0], points[1], ctx.sx, ctx.sy, &values); break;
        case AnnotationKind::Ellipse: ellipseStats(*ctx.frame, points[0], points[1], ctx.sx, ctx.sy, &values); break;
        case AnnotationKind::Freehand: polygonStats(*ctx.frame, points, ctx.sx, ctx.sy, &values); break;
        default: break;
    }
    return values;
}

QStringList Annotation::labelLines(const MeasureContext& ctx) const {
    QStringList lines;
    const bool cal = ctx.calibrated();
    const double sx = cal ? ctx.sx : 1.0;
    const double sy = cal ? ctx.sy : 1.0;
    switch (kind_) {
        case AnnotationKind::Distance:
            if (points.size() >= 2) {
                lines << formatLength(distanceMm(points[0], points[1], sx, sy), cal, ctx.spacing);
            }
            break;
        case AnnotationKind::Angle:
            if (points.size() >= 3) {
                lines << num(angleDegrees(points[0], points[1], points[2], sx, sy), 1) + "°";
            }
            break;
        case AnnotationKind::Cobb:
            if (points.size() >= 4) {
                lines << "Cobb " + num(cobbAngleDegrees(points[0], points[1], points[2], points[3], sx, sy), 1) + "°";
            }
            break;
        case AnnotationKind::Rectangle:
        case AnnotationKind::Ellipse:
        case AnnotationKind::Freehand: {
            if (points.size() < 2 || ctx.frame == nullptr) {
                break;
            }
            RoiStatistics s;
            if (kind_ == AnnotationKind::Rectangle) {
                s = rectangleStats(*ctx.frame, points[0], points[1], sx, sy);
            } else if (kind_ == AnnotationKind::Ellipse) {
                s = ellipseStats(*ctx.frame, points[0], points[1], sx, sy);
            } else {
                s = polygonStats(*ctx.frame, points, sx, sy);
            }
            if (cal) {
                lines << (s.areaMm2 >= 100.0 ? "Área: " + num(s.areaMm2 / 100.0, 2) + " cm²"
                                             : "Área: " + num(s.areaMm2, 1) + " mm²");
            } else {
                lines << "Área: " + num(s.areaMm2, 0) + " px²";
            }
            if (kind_ != AnnotationKind::Freehand) {
                lines << formatLength(s.widthMm, cal, ctx.spacing) + " × " +
                             formatLength(s.heightMm, cal, ctx.spacing);
            } else {
                lines << "Perímetro: " + formatLength(s.perimeterMm, cal, ctx.spacing);
            }
            if (s.count > 0) {
                const QString u = ctx.unit.isEmpty() ? QString() : " " + ctx.unit;
                lines << "Média: " + num(s.mean, 1) + u + "   DP: " + num(s.stdDev, 1);
                lines << "Mín: " + num(s.min, 0) + "   Máx: " + num(s.max, 0) + "   n=" +
                             QLocale().toString(static_cast<qulonglong>(s.count));
            }
            break;
        }
    }
    return lines;
}

void Annotation::paint(QPainter& p, const QTransform& toScreen, const MeasureContext& ctx, bool selected) const {
    if (points.empty()) {
        return;
    }
    const QColor color = selected ? Theme::measurementSelected() : Theme::measurement();
    QPen shadow(QColor(0, 0, 0, 170), 3.2);
    shadow.setCosmetic(true);
    QPen pen(color, 1.5);
    pen.setCosmetic(true);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    auto drawPolyline = [&](const std::vector<Point2>& pts) {
        if (pts.size() < 2) {
            return;
        }
        QPolygonF poly;
        for (const auto& pt : pts) {
            poly << toScreen.map(toQ(pt));
        }
        p.setPen(shadow);
        p.drawPolyline(poly);
        p.setPen(pen);
        p.drawPolyline(poly);
    };

    switch (kind_) {
        case AnnotationKind::Distance:
            drawPolyline(points);
            break;
        case AnnotationKind::Angle:
            drawPolyline(points);
            break;
        case AnnotationKind::Cobb:
            drawPolyline(std::vector<Point2>(points.begin(), points.begin() + std::min<size_t>(2, points.size())));
            if (points.size() >= 3) {
                drawPolyline(std::vector<Point2>(points.begin() + 2, points.end()));
            }
            break;
        case AnnotationKind::Rectangle:
        case AnnotationKind::Ellipse:
        case AnnotationKind::Freehand:
            drawPolyline(finished || kind_ != AnnotationKind::Freehand ? outline(kind_, points) : points);
            break;
    }

    // Handles
    const double hs = selected ? 3.5 : 2.0;
    p.setPen(Qt::NoPen);
    if (kind_ != AnnotationKind::Freehand || selected) {
        const auto& handlePts = kind_ == AnnotationKind::Freehand ? std::vector<Point2>{} : points;
        for (const auto& pt : handlePts) {
            const QPointF s = toScreen.map(toQ(pt));
            p.setBrush(QColor(0, 0, 0, 170));
            p.drawRect(QRectF(s.x() - hs - 1, s.y() - hs - 1, 2 * hs + 2, 2 * hs + 2));
            p.setBrush(color);
            p.drawRect(QRectF(s.x() - hs, s.y() - hs, 2 * hs, 2 * hs));
        }
    }

    // Label
    const QStringList lines = labelLines(ctx);
    if (!lines.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        const QFontMetricsF fm(f);
        double w = 0.0;
        for (const auto& l : lines) {
            w = std::max(w, fm.horizontalAdvance(l));
        }
        const double lh = fm.height();
        const QPointF anchor = toScreen.map(toQ(labelAnchor()));
        const QPointF topLeft = anchor + labelOffset;
        labelRect_ = QRectF(topLeft, QSizeF(w + 10.0, lh * lines.size() + 6.0));
        QPen connector(QColor(color.red(), color.green(), color.blue(), 150), 1.0, Qt::DashLine);
        connector.setCosmetic(true);
        p.setPen(connector);
        p.drawLine(anchor, labelRect_.center().x() < anchor.x() ? labelRect_.topRight() : labelRect_.topLeft());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 165));
        p.drawRoundedRect(labelRect_, 3, 3);
        p.setPen(color);
        for (int i = 0; i < lines.size(); ++i) {
            p.drawText(QPointF(labelRect_.left() + 5.0, labelRect_.top() + 3.0 + fm.ascent() + i * lh), lines[i]);
        }
    } else {
        labelRect_ = QRectF();
    }
    p.restore();
}

int Annotation::hitPoint(const QPointF& pos, const QTransform& toScreen, double tol) const {
    if (kind_ == AnnotationKind::Freehand) {
        return -1;  // freehand contours move as a whole
    }
    for (int i = 0; i < static_cast<int>(points.size()); ++i) {
        const QPointF s = toScreen.map(toQ(points[static_cast<size_t>(i)]));
        if (std::hypot(s.x() - pos.x(), s.y() - pos.y()) <= tol) {
            return i;
        }
    }
    return -1;
}

bool Annotation::hitBody(const QPointF& pos, const QTransform& toScreen, double tol) const {
    std::vector<Point2> pts;
    if (kind_ == AnnotationKind::Cobb && points.size() >= 4) {
        const QPointF a = toScreen.map(toQ(points[0]));
        const QPointF b = toScreen.map(toQ(points[1]));
        const QPointF c = toScreen.map(toQ(points[2]));
        const QPointF d = toScreen.map(toQ(points[3]));
        return segmentDistance(pos, a, b) <= tol || segmentDistance(pos, c, d) <= tol;
    }
    pts = isRoi() ? outline(kind_, points) : points;
    for (size_t i = 1; i < pts.size(); ++i) {
        if (segmentDistance(pos, toScreen.map(toQ(pts[i - 1])), toScreen.map(toQ(pts[i]))) <= tol) {
            return true;
        }
    }
    return false;
}

}  // namespace vtc
