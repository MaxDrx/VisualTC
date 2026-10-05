#include "ui/SeriesBrowser.h"

#include <QContextMenuEvent>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QStyledItemDelegate>

#include "app/Theme.h"
#include "dicom/TextUtil.h"

namespace vtc {

namespace {

enum Role { SeriesIdRole = Qt::UserRole, KindRole, TitleRole, DetailRole, WarningRole, DisplayedRole };
constexpr int kThumb = 64;

class SeriesDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        if (index.data(KindRole).toInt() == 0) {
            return {option.rect.width(), 46};
        }
        return {option.rect.width(), kThumb + 12};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto& c = Theme::colors();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        if (index.data(KindRole).toInt() == 0) {
            // Study header
            p->setPen(c.accent);
            QFont f = option.font;
            f.setBold(true);
            p->setFont(f);
            p->drawText(r.adjusted(4, 2, -4, 0), Qt::AlignLeft | Qt::AlignTop, index.data(TitleRole).toString());
            f.setBold(false);
            p->setFont(f);
            p->setPen(c.textSecondary);
            p->drawText(r.adjusted(4, 0, -4, -2), Qt::AlignLeft | Qt::AlignBottom, index.data(DetailRole).toString());
            p->restore();
            return;
        }
        if (selected || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? c.accentDim : c.panel);
            p->drawRoundedRect(r, 5, 5);
        }
        const QRect thumbRect(r.left() + 4, r.top() + (r.height() - kThumb) / 2, kThumb, kThumb);
        const QVariant deco = index.data(Qt::DecorationRole);
        p->fillRect(thumbRect, Qt::black);
        if (deco.canConvert<QImage>()) {
            p->drawImage(thumbRect, deco.value<QImage>());
        }
        if (index.data(DisplayedRole).toBool()) {
            p->setPen(QPen(c.accent, 2));
            p->setBrush(Qt::NoBrush);
            p->drawRect(thumbRect.adjusted(1, 1, -1, -1));
        } else {
            p->setPen(QPen(c.panelBorder, 1));
            p->setBrush(Qt::NoBrush);
            p->drawRect(thumbRect);
        }
        const QRect text = r.adjusted(kThumb + 12, 4, -4, -4);
        QFont f = option.font;
        p->setFont(f);
        p->setPen(c.text);
        const QFontMetrics fm(f);
        p->drawText(text.left(), text.top() + fm.ascent(),
                    fm.elidedText(index.data(TitleRole).toString(), Qt::ElideRight, text.width()));
        p->setPen(c.textSecondary);
        const QStringList details = index.data(DetailRole).toString().split('\n');
        int y = text.top() + fm.ascent() + fm.height();
        for (const auto& d : details) {
            p->drawText(text.left(), y, fm.elidedText(d, Qt::ElideRight, text.width()));
            y += fm.height();
        }
        const QString warn = index.data(WarningRole).toString();
        if (!warn.isEmpty()) {
            p->setPen(c.warning);
            p->drawText(text.left(), y, fm.elidedText("⚠ " + warn, Qt::ElideRight, text.width()));
        }
        p->restore();
    }
};

QString shortWarning(const StackGeometry& g) {
    if (g.has(GeometryIssue::DuplicatePositions)) {
        return QObject::tr("cortes duplicados");
    }
    if (g.has(GeometryIssue::MissingSlices)) {
        return QObject::tr("cortes ausentes");
    }
    if (g.has(GeometryIssue::GantryTilt)) {
        return QObject::tr("gantry tilt %1°").arg(QLocale().toString(g.tiltDegrees, 'f', 1));
    }
    if (g.has(GeometryIssue::IrregularSpacing)) {
        return QObject::tr("espaçamento irregular");
    }
    return {};
}

}  // namespace

SeriesBrowser::SeriesBrowser(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setColumnCount(1);
    setRootIsDecorated(false);
    setIndentation(0);
    setItemDelegate(new SeriesDelegate(this));
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setMouseTracking(true);
    setUniformRowHeights(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // itemActivated covers double click (or single click, when that is the
    // platform convention) and Enter; also listening to itemDoubleClicked
    // would open the series twice.
    connect(this, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const QString id = item->data(0, SeriesIdRole).toString();
        if (!id.isEmpty()) {
            Q_EMIT seriesActivated(id);
        }
    });
}

void SeriesBrowser::setDatabase(const StudyDatabase& db) {
    std::map<QString, QVariant> oldThumbs;
    for (const auto& [id, item] : items_) {
        oldThumbs[id] = item->data(0, Qt::DecorationRole);
    }
    clear();
    items_.clear();
    const QLocale loc;
    for (const auto& patient : db.patients()) {
        for (const auto& study : patient->studies) {
            auto* header = new QTreeWidgetItem(this);
            header->setData(0, KindRole, 0);
            header->setFlags(Qt::ItemIsEnabled);
            const QString name = QString::fromStdString(patient->name.empty() ? "(sem nome)" : patient->name);
            header->setData(0, TitleRole, name);
            QString detail = QString::fromStdString(formatDicomDate(study->date));
            if (!study->description.empty()) {
                detail += "  ·  " + QString::fromStdString(study->description);
            }
            header->setData(0, DetailRole, detail);
            header->setToolTip(0, tr("Paciente: %1\nID: %2\nEstudo: %3\nInstituição: %4")
                                      .arg(name, QString::fromStdString(patient->id),
                                           QString::fromStdString(study->description),
                                           QString::fromStdString(study->institution)));
            for (const auto& series : study->series) {
                auto* item = new QTreeWidgetItem(this);
                const QString id = QString::fromStdString(series->id);
                item->setData(0, SeriesIdRole, id);
                item->setData(0, KindRole, 1);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
                QString title = series->number() ? tr("Série %1").arg(*series->number()) : tr("Série");
                const QString desc = QString::fromStdString(series->description());
                if (!desc.isEmpty()) {
                    title += " · " + desc;
                }
                item->setData(0, TitleRole, title);
                QString line1 = QString::fromStdString(series->modality()) + "  ·  " +
                                tr("%n imagem(ns)", "", series->frameCount());
                QString line2;
                if (auto t = series->sliceThickness()) {
                    line2 = tr("Espessura %1 mm").arg(loc.toString(*t, 'f', 2));
                }
                if (series->geometry.volumetric) {
                    line2 += (line2.isEmpty() ? "" : "  ·  ") + tr("MPR");
                }
                item->setData(0, DetailRole, line2.isEmpty() ? line1 : line1 + "\n" + line2);
                item->setData(0, WarningRole, shortWarning(series->geometry));
                QStringList tip;
                tip << title << line1;
                if (!line2.isEmpty()) {
                    tip << line2;
                }
                for (auto issue : series->geometry.issues) {
                    tip << QString::fromStdString(describe(issue));
                }
                item->setToolTip(0, tip.join('\n'));
                if (auto it = oldThumbs.find(id); it != oldThumbs.end()) {
                    item->setData(0, Qt::DecorationRole, it->second);
                }
                items_[id] = item;
            }
        }
    }
}

void SeriesBrowser::setThumbnail(const QString& seriesId, const QImage& image) {
    if (auto it = items_.find(seriesId); it != items_.end()) {
        it->second->setData(0, Qt::DecorationRole, image);
    }
}

void SeriesBrowser::markDisplayed(const QSet<QString>& seriesIds) {
    for (auto& [id, item] : items_) {
        item->setData(0, DisplayedRole, seriesIds.contains(id));
    }
    viewport()->update();
}

QString SeriesBrowser::currentSeriesId() const {
    auto* item = currentItem();
    return item != nullptr ? item->data(0, SeriesIdRole).toString() : QString();
}

QStringList SeriesBrowser::mimeTypes() const { return {QStringLiteral("application/x-visualtc-series")}; }

QMimeData* SeriesBrowser::mimeData(const QList<QTreeWidgetItem*>& items) const {
    if (items.isEmpty()) {
        return nullptr;
    }
    const QString id = items.front()->data(0, SeriesIdRole).toString();
    if (id.isEmpty()) {
        return nullptr;
    }
    auto* mime = new QMimeData;
    mime->setData("application/x-visualtc-series", id.toUtf8());
    return mime;
}

void SeriesBrowser::contextMenuEvent(QContextMenuEvent* event) {
    QTreeWidgetItem* item = itemAt(event->pos());
    if (item == nullptr) {
        return;
    }
    const QString id = item->data(0, SeriesIdRole).toString();
    if (id.isEmpty()) {
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Abrir no viewport ativo"), this, [this, id] { Q_EMIT seriesActivated(id); });
    menu.addAction(tr("Abrir em MPR"), this, [this, id] { Q_EMIT seriesMprRequested(id); });
    menu.addSeparator();
    menu.addAction(tr("Informações DICOM…"), this, [this, id] { Q_EMIT seriesInfoRequested(id); });
    menu.exec(event->globalPos());
}

}  // namespace vtc
