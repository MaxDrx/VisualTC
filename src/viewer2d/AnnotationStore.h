#pragma once

#include <QObject>
#include <QUndoStack>
#include <map>
#include <string>
#include <vector>

#include "viewer2d/Annotations.h"

namespace vtc {

// Session-wide annotations, keyed by image (file + frame, or MPR plane).
// The same image shown in two viewports shows the same measurements. Every
// change goes through a QUndoStack. Annotations are kept separately from the
// DICOM files, which are never modified (section 75).
class AnnotationStore : public QObject {
    Q_OBJECT
public:
    explicit AnnotationStore(QObject* parent = nullptr);

    [[nodiscard]] const std::vector<AnnotationPtr>& list(const std::string& key) const;
    void add(const std::string& key, const AnnotationPtr& a);
    void remove(const std::string& key, const AnnotationPtr& a);
    void clearImage(const std::string& key);
    void clearAll();
    // Records a geometry change done interactively (for undo).
    void recordEdit(const std::string& key, const AnnotationPtr& a, std::vector<Point2> before, QPointF labelBefore);
    [[nodiscard]] int totalCount() const;

    QUndoStack* undoStack() { return &undo_; }

    // Raw operations used by the undo commands.
    void insertRaw(const std::string& key, const AnnotationPtr& a, int index = -1);
    int removeRaw(const std::string& key, const AnnotationPtr& a);
    void notifyChanged() { Q_EMIT changed(); }

Q_SIGNALS:
    void changed();

private:
    std::map<std::string, std::vector<AnnotationPtr>> data_;
    QUndoStack undo_;
};

}  // namespace vtc
