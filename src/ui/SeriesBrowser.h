#pragma once

#include <QTreeWidget>
#include <map>

#include "dicom/DicomStudy.h"

namespace vtc {

// Patient/study headers with series thumbnails underneath. Series can be
// double-clicked (open in the active viewport) or dragged onto a viewport.
class SeriesBrowser : public QTreeWidget {
    Q_OBJECT
public:
    explicit SeriesBrowser(QWidget* parent = nullptr);

    void setDatabase(const StudyDatabase& db);
    void setThumbnail(const QString& seriesId, const QImage& image);
    [[nodiscard]] QString currentSeriesId() const;
    [[nodiscard]] int seriesCount() const { return static_cast<int>(items_.size()); }
    void markDisplayed(const QSet<QString>& seriesIds);
    // Centre of the "×" of the n-th study header (viewport coordinates; tests).
    [[nodiscard]] QPoint closeButtonCenter(int studyIndex) const;

Q_SIGNALS:
    void seriesActivated(const QString& seriesId);
    void seriesMprRequested(const QString& seriesId);
    void seriesInfoRequested(const QString& seriesId);
    void studyCloseRequested(const QString& patientKey, const QString& studyKey);

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QTreeWidgetItem* closeButtonAt(const QPoint& pos) const;
    void setCloseHover(QTreeWidgetItem* item);

    std::map<QString, QTreeWidgetItem*> items_;
    QTreeWidgetItem* closeHover_ = nullptr;
};

}  // namespace vtc
