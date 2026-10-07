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
    // Reference lines currently drawn (0: nothing comparable on screen).
    [[nodiscard]] int referenceLineCount() const { return refLineCount_; }
    void setTool(Tool tool);
    void setOverlaysVisible(bool on);
    void setSmooth(bool on);
    void setEmptyHint(const QString& text);
    void clearAll();

    // MPR
    void enterMpr(const std::shared_ptr<MprSession>& session);
    void exitMpr();
    [[nodiscard]] const std::shared_ptr<MprSession>& mprSession() const { return mpr_; }
    // Crosshair lines of the MPR ("Cruz" button): hidden lines are neither
    // drawn nor draggable.
    void setMprGuidesVisible(bool on);
    [[nodiscard]] bool mprGuidesVisible() const { return guidesVisible_; }

Q_SIGNALS:
    void activeViewportChanged(Viewport* vp);
    void activeStateChanged(Viewport* vp);  // window, slice, transform of the active viewport
    void selectionChanged();                // selected annotation of the active viewport
    void cursorInfo(const QString& text);
    void seriesDropped(Viewport* vp, const QString& seriesId);
    void displayedSeriesChanged();

private:
    void relayout();
    void onSliceChanged(Viewport* vp);
    void onWindowChanged(Viewport* vp);
    void onViewChanged(Viewport* vp);
    void onCrosshairDragged(Viewport* vp, const Vec3& point);
    void onMprLineDragged(Viewport* vp, int plane, const Vec3& point);
    void onMprRotateDragged(Viewport* vp, const Vec3& from, const Vec3& to);
    void onMprSlabDragged(Viewport* vp, int plane, double thicknessMm);
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
    int refLineCount_ = 0;
    bool guidesVisible_ = true;
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
