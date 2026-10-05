#pragma once

#include <QGridLayout>
#include <QWidget>
#include <map>
#include <memory>
#include <vector>

#include "mpr/MprSession.h"
#include "viewer2d/Viewport.h"

namespace vtc {

class AnnotationStore;

// The central area: a grid of up to 3x3 viewports. Owns layout changes,
// active viewport, maximize/restore, spatial synchronization, reference
// lines and the MPR crosshair binding.
class ViewerGrid : public QWidget {
    Q_OBJECT
public:
    explicit ViewerGrid(AnnotationStore* store, QWidget* parent = nullptr);

    void setLayoutGrid(int rows, int cols);
    [[nodiscard]] int rows() const { return rows_; }
    [[nodiscard]] int cols() const { return cols_; }
    [[nodiscard]] Viewport* activeViewport() const { return active_; }
    [[nodiscard]] std::vector<Viewport*> visibleViewports() const;
    [[nodiscard]] Viewport* firstEmptyViewport() const;
    void setActive(Viewport* vp);
    void activateNext();

    // Shows a source in a viewport, remembering/restoring per-series state.
    void showSource(Viewport* vp, std::shared_ptr<ImageSource> source);
    void toggleMaximize(Viewport* vp);
    [[nodiscard]] bool isMaximized() const { return maximized_ != nullptr; }

    void setSyncEnabled(bool on);
    [[nodiscard]] bool syncEnabled() const { return sync_; }
    // Explains why no viewport could be synchronized (empty when OK).
    [[nodiscard]] QString syncProblem() const;
    void setReferenceLinesEnabled(bool on);
    void setTool(Tool tool);
    void setOverlaysVisible(bool on);
    void setSmooth(bool on);
    void clearAll();

    // MPR
    void enterMpr(const std::shared_ptr<MprSession>& session);
    void exitMpr();
    [[nodiscard]] const std::shared_ptr<MprSession>& mprSession() const { return mpr_; }

Q_SIGNALS:
    void activeViewportChanged(Viewport* vp);
    void activeStateChanged(Viewport* vp);  // window, slice, transform of the active viewport
    void cursorInfo(const QString& text);
    void seriesDropped(Viewport* vp, const QString& seriesId);
    void displayedSeriesChanged();

private:
    void relayout();
    void onSliceChanged(Viewport* vp);
    void onWindowChanged(Viewport* vp);
    void onViewChanged(Viewport* vp);
    void onCrosshairDragged(Viewport* vp, const Vec3& point);
    void onMprCenterChanged();
    void updateReferenceLines();
    void updateCrosshairs();
    void syncFrom(Viewport* vp);

    AnnotationStore* store_;
    QGridLayout* grid_;
    std::vector<Viewport*> viewports_;
    int rows_ = 1;
    int cols_ = 1;
    Viewport* active_ = nullptr;
    Viewport* maximized_ = nullptr;
    bool sync_ = false;
    bool refLines_ = true;
    bool busy_ = false;  // re-entrancy guard for synchronization
    std::map<std::string, ViewState> savedStates_;

    std::shared_ptr<MprSession> mpr_;
    struct Saved {
        int rows = 1;
        int cols = 1;
        std::vector<std::shared_ptr<ImageSource>> sources;
        Tool tool = Tool::WindowLevel;
    } beforeMpr_;
    Tool tool_ = Tool::WindowLevel;
};

}  // namespace vtc
