#include "viewer2d/AnnotationStore.h"

#include <algorithm>

namespace vtc {

namespace {

class AddCommand : public QUndoCommand {
public:
    AddCommand(AnnotationStore* s, std::string key, AnnotationPtr a)
        : QUndoCommand(QObject::tr("Adicionar medida")), s_(s), key_(std::move(key)), a_(std::move(a)) {}
    void redo() override { s_->insertRaw(key_, a_); }
    void undo() override { s_->removeRaw(key_, a_); }

private:
    AnnotationStore* s_;
    std::string key_;
    AnnotationPtr a_;
};

class RemoveCommand : public QUndoCommand {
public:
    RemoveCommand(AnnotationStore* s, std::string key, AnnotationPtr a)
        : QUndoCommand(QObject::tr("Excluir medida")), s_(s), key_(std::move(key)), a_(std::move(a)) {}
    void redo() override { index_ = s_->removeRaw(key_, a_); }
    void undo() override { s_->insertRaw(key_, a_, index_); }

private:
    AnnotationStore* s_;
    std::string key_;
    AnnotationPtr a_;
    int index_ = -1;
};

class EditCommand : public QUndoCommand {
public:
    EditCommand(AnnotationStore* s, AnnotationPtr a, std::vector<Point2> before, QPointF labelBefore)
        : QUndoCommand(QObject::tr("Editar medida")),
          s_(s),
          a_(std::move(a)),
          before_(std::move(before)),
          after_(a_->points),
          labelBefore_(labelBefore),
          labelAfter_(a_->labelOffset) {}
    void redo() override {
        a_->points = after_;
        a_->labelOffset = labelAfter_;
        s_->notifyChanged();
    }
    void undo() override {
        a_->points = before_;
        a_->labelOffset = labelBefore_;
        s_->notifyChanged();
    }

private:
    AnnotationStore* s_;
    AnnotationPtr a_;
    std::vector<Point2> before_;
    std::vector<Point2> after_;
    QPointF labelBefore_;
    QPointF labelAfter_;
};

}  // namespace

AnnotationStore::AnnotationStore(QObject* parent) : QObject(parent) {}

const std::vector<AnnotationPtr>& AnnotationStore::list(const std::string& key) const {
    static const std::vector<AnnotationPtr> empty;
    const auto it = data_.find(key);
    return it == data_.end() ? empty : it->second;
}

void AnnotationStore::insertRaw(const std::string& key, const AnnotationPtr& a, int index) {
    auto& v = data_[key];
    if (index < 0 || index > static_cast<int>(v.size())) {
        v.push_back(a);
    } else {
        v.insert(v.begin() + index, a);
    }
    Q_EMIT changed();
}

int AnnotationStore::removeRaw(const std::string& key, const AnnotationPtr& a) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return -1;
    }
    auto& v = it->second;
    const auto pos = std::find(v.begin(), v.end(), a);
    if (pos == v.end()) {
        return -1;
    }
    const int index = static_cast<int>(std::distance(v.begin(), pos));
    v.erase(pos);
    Q_EMIT changed();
    return index;
}

void AnnotationStore::add(const std::string& key, const AnnotationPtr& a) {
    undo_.push(new AddCommand(this, key, a));
}

void AnnotationStore::remove(const std::string& key, const AnnotationPtr& a) {
    undo_.push(new RemoveCommand(this, key, a));
}

void AnnotationStore::clearImage(const std::string& key) {
    const auto copy = list(key);
    if (copy.empty()) {
        return;
    }
    undo_.beginMacro(tr("Excluir medidas da imagem"));
    for (const auto& a : copy) {
        remove(key, a);
    }
    undo_.endMacro();
}

void AnnotationStore::clearAll() {
    if (totalCount() == 0) {
        return;
    }
    undo_.beginMacro(tr("Excluir todas as medidas"));
    const auto snapshot = data_;
    for (const auto& [key, v] : snapshot) {
        for (const auto& a : v) {
            remove(key, a);
        }
    }
    undo_.endMacro();
}

void AnnotationStore::recordEdit(const std::string& /*key*/, const AnnotationPtr& a, std::vector<Point2> before,
                                 QPointF labelBefore) {
    undo_.push(new EditCommand(this, a, std::move(before), labelBefore));
}

int AnnotationStore::totalCount() const {
    int n = 0;
    for (const auto& [k, v] : data_) {
        n += static_cast<int>(v.size());
    }
    return n;
}

}  // namespace vtc
