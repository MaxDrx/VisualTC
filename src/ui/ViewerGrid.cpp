#include "ui/ViewerGrid.h"

#include <cmath>
#include <numbers>

#include "app/AppSettings.h"
#include "app/Theme.h"
#include "mpr/MprSource.h"
#include "viewer2d/StackSource.h"

namespace vtc {

namespace {
constexpr int kMaxViewports = 9;

StackSource* asStack(Viewport* vp) { return vp && vp->source() ? dynamic_cast<StackSource*>(vp->source().get()) : nullptr; }
MprSource* asMpr(Viewport* vp) { return vp && vp->source() ? dynamic_cast<MprSource*>(vp->source().get()) : nullptr; }
}  // namespace

ViewerGrid::ViewerGrid(AnnotationStore* store, QWidget* parent) : QWidget(parent), store_(store) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(2);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor("#2A2A2A"));
    setPalette(pal);
    refLines_ = AppSettings::instance().referenceLines();
    for (int i = 0; i < kMaxViewports; ++i) {
        auto* vp = new Viewport(store_, this);
        viewports_.push_back(vp);
        connect(vp, &Viewport::activated, this, &ViewerGrid::setActive);
        connect(vp, &Viewport::sliceChanged, this, &ViewerGrid::onSliceChanged);
        connect(vp, &Viewport::windowChanged, this, &ViewerGrid::onWindowChanged);
        connect(vp, &Viewport::viewTransformChanged, this, &ViewerGrid::onViewChanged);
        connect(vp, &Viewport::maximizeRequested, this, &ViewerGrid::toggleMaximize);
        connect(vp, &Viewport::cursorInfo, this, &ViewerGrid::cursorInfo);
        connect(vp, &Viewport::crosshairDragged, this, &ViewerGrid::onCrosshairDragged);
        connect(vp, &Viewport::mprLineDragged, this, &ViewerGrid::onMprLineDragged);
        connect(vp, &Viewport::mprRotateDragged, this, &ViewerGrid::onMprRotateDragged);
        connect(vp, &Viewport::mprSlabDragged, this, &ViewerGrid::onMprSlabDragged);
        connect(vp, &Viewport::seriesDropped, this, &ViewerGrid::seriesDropped);
        connect(vp, &Viewport::annotationSelected, this, [this](Viewport* v) {
            if (v == active_) {
                Q_EMIT selectionChanged();
            }
        });
    }
    active_ = viewports_.front();
    active_->setActive(true);
    relayout();
}

std::vector<Viewport*> ViewerGrid::visibleViewports() const {
    std::vector<Viewport*> out;
    if (maximized_ != nullptr) {
        out.push_back(maximized_);
        return out;
    }
    for (int i = 0; i < rows_ * cols_; ++i) {
        out.push_back(viewports_[static_cast<size_t>(i)]);
    }
    return out;
}

Viewport* ViewerGrid::firstEmptyViewport() const {
    for (auto* vp : visibleViewports()) {
        if (!vp->source()) {
            return vp;
        }
    }
    return nullptr;
}

void ViewerGrid::relayout() {
    for (auto* vp : viewports_) {
        grid_->removeWidget(vp);
        vp->hide();
    }
    for (int r = 0; r < 3; ++r) {
        grid_->setRowStretch(r, 0);
    }
    for (int c = 0; c < 3; ++c) {
        grid_->setColumnStretch(c, 0);
    }
    if (maximized_ != nullptr) {
        grid_->addWidget(maximized_, 0, 0);
        grid_->setRowStretch(0, 1);
        grid_->setColumnStretch(0, 1);
        maximized_->show();
    } else {
        for (int i = 0; i < rows_ * cols_; ++i) {
            auto* vp = viewports_[static_cast<size_t>(i)];
            grid_->addWidget(vp, i / cols_, i % cols_);
            vp->show();
        }
        for (int r = 0; r < rows_; ++r) {
            grid_->setRowStretch(r, 1);
        }
        for (int c = 0; c < cols_; ++c) {
            grid_->setColumnStretch(c, 1);
        }
    }
    const bool multi = maximized_ == nullptr && rows_ * cols_ > 1;
    for (auto* vp : viewports_) {
        vp->setShowActiveFrame(multi);
        if (vp->isHidden() && vp->cinePlaying()) {
            vp->setCinePlaying(false);  // no invisible playback decoding in the background
        }
    }
    const auto visible = visibleViewports();
    if (std::find(visible.begin(), visible.end(), active_) == visible.end()) {
        setActive(visible.front());
    }
    updateReferenceLines();
}

void ViewerGrid::setLayoutGrid(int rows, int cols) {
    rows_ = std::clamp(rows, 1, 3);
    cols_ = std::clamp(cols, 1, 3);
    maximized_ = nullptr;
    relayout();
    Q_EMIT displayedSeriesChanged();
}

void ViewerGrid::setActive(Viewport* vp) {
    if (vp == nullptr) {
        return;
    }
    if (active_ != vp) {
        if (active_ != nullptr) {
            active_->setActive(false);
            active_->cancelPendingAnnotation();
        }
        active_ = vp;
        active_->setActive(true);
        updateReferenceLines();
        Q_EMIT activeViewportChanged(vp);
        Q_EMIT selectionChanged();
    }
}

void ViewerGrid::activateNext() {
    const auto visible = visibleViewports();
    auto it = std::find(visible.begin(), visible.end(), active_);
    if (it == visible.end() || ++it == visible.end()) {
        it = visible.begin();
    }
    setActive(*it);
}

void ViewerGrid::showSource(Viewport* vp, std::shared_ptr<ImageSource> source) {
    if (vp == nullptr) {
        return;
    }
    if (vp->source()) {
        savedStates_[vp->source()->stateKey()] = vp->viewState();
    }
    const ViewState* restore = nullptr;
    if (source) {
        const auto it = savedStates_.find(source->stateKey());
        if (it != savedStates_.end()) {
            restore = &it->second;
        }
    }
    vp->setSource(std::move(source), restore);
    vp->setTool(tool_);
    if (sync_) {
        syncFrom(active_);
    }
    updateReferenceLines();
    updateCrosshairs();
    Q_EMIT displayedSeriesChanged();
    if (vp == active_) {
        Q_EMIT activeStateChanged(vp);
    }
}

void ViewerGrid::toggleMaximize(Viewport* vp) {
    maximized_ = maximized_ != nullptr ? nullptr : vp;
    if (maximized_ != nullptr) {
        setActive(vp);
    }
    relayout();
}

void ViewerGrid::setSyncEnabled(bool on) {
    sync_ = on;
    if (sync_) {
        syncFrom(active_);
    }
}

QString ViewerGrid::syncProblem() const {
    const auto visible = visibleViewports();
    int stacks = 0;
    bool anyPair = false;
    bool sameForButNotParallel = false;
    bool differentFor = false;
    for (size_t i = 0; i < visible.size(); ++i) {
        auto* a = visible[i];
        if (!asStack(a)) {
            continue;
        }
        ++stacks;
        const auto pa = a->currentPlane();
        for (size_t j = i + 1; j < visible.size(); ++j) {
            auto* b = visible[j];
            if (!asStack(b)) {
                continue;
            }
            const auto pb = b->currentPlane();
            if (!pa || !pb) {
                continue;
            }
            if (!spatiallyComparable(*pa, *pb)) {
                differentFor = true;
                continue;
            }
            if (!planesParallel(pa->geometry.normal(), pb->geometry.normal(), 20.0)) {
                sameForButNotParallel = true;
                continue;
            }
            anyPair = true;
        }
    }
    if (anyPair || stacks < 2) {
        return {};
    }
    if (differentFor) {
        return tr("As séries exibidas pertencem a sistemas de coordenadas diferentes (Frame of Reference) e não podem "
                  "ser sincronizadas espacialmente.");
    }
    if (sameForButNotParallel) {
        return tr("As séries exibidas não são paralelas; use as linhas de referência para correlacionar os planos.");
    }
    return tr("As séries exibidas não possuem geometria espacial suficiente para sincronização.");
}

void ViewerGrid::setReferenceLinesEnabled(bool on) {
    refLines_ = on;
    updateReferenceLines();
}

void ViewerGrid::setTool(Tool tool) {
    tool_ = tool;
    for (auto* vp : viewports_) {
        vp->setTool(tool);
    }
}

void ViewerGrid::setOverlaysVisible(bool on) {
    for (auto* vp : viewports_) {
        vp->setOverlaysVisible(on);
    }
}

void ViewerGrid::setSmooth(bool on) {
    for (auto* vp : viewports_) {
        vp->setSmooth(on);
    }
}

void ViewerGrid::setEmptyHint(const QString& text) {
    for (auto* vp : viewports_) {
        vp->setEmptyHint(text);
    }
}

void ViewerGrid::clearAll() {
    exitMpr();
    for (auto* vp : viewports_) {
        vp->setSource(nullptr);
    }
    savedStates_.clear();
    Q_EMIT displayedSeriesChanged();
}

// ------------------------------------------------------------------ sync

void ViewerGrid::onSliceChanged(Viewport* vp) {
    if (busy_) {
        return;
    }
    if (auto* ms = asMpr(vp); ms != nullptr && ms->session() == mpr_ && mpr_) {
        busy_ = true;
        mpr_->moveToSlice(ms->orientation(), vp->sliceIndex());
        busy_ = false;
        onMprCenterChanged();
    } else if (sync_) {
        syncFrom(vp);
    }
    updateReferenceLines();
    if (vp == active_) {
        Q_EMIT activeStateChanged(vp);
    }
}

void ViewerGrid::syncFrom(Viewport* vp) {
    if (vp == nullptr || busy_) {
        return;
    }
    const auto plane = vp->currentPlane();
    if (!plane) {
        return;
    }
    const Vec3 c = plane->center();
    const Vec3 n = plane->geometry.normal();
    busy_ = true;
    for (auto* w : visibleViewports()) {
        if (w == vp) {
            continue;
        }
        auto* ss = asStack(w);
        if (ss == nullptr) {
            continue;
        }
        const auto wp = w->currentPlane();
        if (!wp || !spatiallyComparable(*plane, *wp) || !planesParallel(n, wp->geometry.normal(), 20.0)) {
            continue;
        }
        std::vector<FrameGeometry> geoms;
        geoms.reserve(ss->series()->frames.size());
        for (const auto& f : ss->series()->frames) {
            geoms.push_back(f.geometry());
        }
        const double tol = std::max(5.0, 2.0 * ss->series()->geometry.sliceSpacing);
        if (auto idx = nearestSliceIndex(geoms, c, tol)) {
            w->setSliceIndex(*idx);
        }
    }
    busy_ = false;
}

void ViewerGrid::onWindowChanged(Viewport* vp) {
    if (vp == active_) {
        Q_EMIT activeStateChanged(vp);
    }
    if (busy_) {
        return;
    }
    const bool mprLinked = asMpr(vp) != nullptr && mpr_;
    const bool syncWindow = sync_ && AppSettings::instance().syncWindow();
    if (!mprLinked && !syncWindow) {
        return;
    }
    busy_ = true;
    for (auto* w : visibleViewports()) {
        if (w == vp || !w->source()) {
            continue;
        }
        const bool sameMpr = mprLinked && asMpr(w) != nullptr && asMpr(w)->session() == mpr_;
        bool sameModality = false;
        if (syncWindow && w->source()->instanceAt(0) && vp->source()->instanceAt(0)) {
            sameModality = w->source()->instanceAt(0)->modality == vp->source()->instanceAt(0)->modality;
        }
        if (sameMpr || sameModality) {
            w->setWindow(vp->windowCenter(), vp->windowWidth());
            w->setInvert(vp->inverted());
        }
    }
    busy_ = false;
}

void ViewerGrid::onViewChanged(Viewport* vp) {
    if (vp == active_) {
        Q_EMIT activeStateChanged(vp);
    }
    if (busy_ || !sync_ || !AppSettings::instance().syncZoomPan()) {
        return;
    }
    busy_ = true;
    for (auto* w : visibleViewports()) {
        if (w != vp && w->source()) {
            w->setZoom(vp->zoom());
            w->setPan(vp->pan());
        }
    }
    busy_ = false;
}

void ViewerGrid::updateReferenceLines() {
    const auto visible = visibleViewports();
    std::optional<PlaneRect> activePlane;
    if (refLines_ && active_ != nullptr) {
        activePlane = active_->currentPlane();
    }
    refLineCount_ = 0;
    for (auto* vp : visible) {
        std::vector<GuideLine> lines;
        if (activePlane && vp != active_ && asMpr(vp) == nullptr) {
            if (const auto target = vp->currentPlane(); target && referenceComparable(*activePlane, *target)) {
                if (auto seg = referenceLine(*activePlane, *target)) {
                    QColor color = Theme::referenceLine();
                    if (auto* ms = asMpr(active_)) {
                        color = MprSource::colorFor(ms->orientation());
                    }
                    lines.push_back({seg->first, seg->second, color, false});
                    ++refLineCount_;
                }
            }
        }
        vp->setReferenceLines(std::move(lines));
    }
}

// ------------------------------------------------------------------- MPR

void ViewerGrid::enterMpr(const std::shared_ptr<MprSession>& session) {
    if (mpr_) {
        exitMpr();
    }
    beforeMpr_.rows = rows_;
    beforeMpr_.cols = cols_;
    beforeMpr_.tool = tool_;
    beforeMpr_.sources.clear();
    for (auto* vp : visibleViewports()) {
        beforeMpr_.sources.push_back(vp->source());
    }
    mpr_ = session;
    connect(mpr_.get(), &MprSession::centerChanged, this, &ViewerGrid::onMprCenterChanged);
    connect(mpr_.get(), &MprSession::renderingChanged, this, [this] {
        onMprCenterChanged();
    });
    setLayoutGrid(1, 3);
    const MprOrientation order[3] = {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal};
    for (int i = 0; i < 3; ++i) {
        showSource(viewports_[static_cast<size_t>(i)], std::make_shared<MprSource>(mpr_, order[i]));
    }
    setTool(Tool::Crosshair);
    setActive(viewports_[0]);
    // Same window on the three planes.
    busy_ = true;
    for (int i = 1; i < 3; ++i) {
        viewports_[static_cast<size_t>(i)]->setWindow(viewports_[0]->windowCenter(), viewports_[0]->windowWidth());
    }
    busy_ = false;
    onMprCenterChanged();
}

void ViewerGrid::exitMpr() {
    if (!mpr_) {
        return;
    }
    disconnect(mpr_.get(), nullptr, this, nullptr);
    for (auto* vp : viewports_) {
        if (auto* ms = asMpr(vp); ms != nullptr && ms->session() == mpr_) {
            vp->setSource(nullptr);
        }
    }
    mpr_.reset();
    setLayoutGrid(beforeMpr_.rows, beforeMpr_.cols);
    const auto visible = visibleViewports();
    for (size_t i = 0; i < visible.size() && i < beforeMpr_.sources.size(); ++i) {
        if (beforeMpr_.sources[i] && !asMpr(visible[i])) {
            showSource(visible[i], beforeMpr_.sources[i]);
        }
    }
    setTool(beforeMpr_.tool == Tool::Crosshair ? Tool::WindowLevel : beforeMpr_.tool);
    Q_EMIT displayedSeriesChanged();
}

void ViewerGrid::onCrosshairDragged(Viewport* vp, const Vec3& point) {
    auto* ms = asMpr(vp);
    if (ms == nullptr || ms->session() != mpr_ || !mpr_) {
        return;
    }
    // Keep the dragged point on the plane currently shown in this view.
    const ReslicePlane plane = ms->planeAt(vp->sliceIndex());
    const Vec3 n = plane.normal();
    const Vec3 onPlane = point - n * (point - plane.origin).dot(n);
    mpr_->setCenter(onPlane);
}

void ViewerGrid::onMprLineDragged(Viewport* vp, int plane, const Vec3& point) {
    auto* ms = asMpr(vp);
    if (ms == nullptr || ms->session() != mpr_ || !mpr_ || plane < 0 || plane > 2) {
        return;
    }
    // Move the dragged plane P through the mouse point while the third plane
    // Q stays where it is: the centre slides along Q's line in this view.
    const auto o = ms->orientation();
    const auto p = static_cast<MprOrientation>(plane);
    MprOrientation q = MprOrientation::Axial;
    for (auto c : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
        if (c != o && c != p) {
            q = c;
        }
    }
    const Vec3 nO = mpr_->view(o).n;
    const Vec3 nP = mpr_->view(p).n;
    const Vec3 dQ = nO.cross(mpr_->view(q).n).normalized();
    const double along = dQ.dot(nP);
    if (std::abs(along) < 1e-6) {
        return;
    }
    const Vec3 c = mpr_->center();
    mpr_->setCenter(c + dQ * ((point - c).dot(nP) / along));
}

void ViewerGrid::onMprRotateDragged(Viewport* vp, const Vec3& from, const Vec3& to) {
    auto* ms = asMpr(vp);
    if (ms == nullptr || ms->session() != mpr_ || !mpr_) {
        return;
    }
    // Angle swept around the crosshair, in this view's plane.
    const Vec3 n = mpr_->view(ms->orientation()).n;
    const Vec3 c = mpr_->center();
    Vec3 a = from - c;
    Vec3 b = to - c;
    a = a - n * a.dot(n);
    b = b - n * b.dot(n);
    if (a.norm() < 1e-6 || b.norm() < 1e-6) {
        return;
    }
    const double angle = std::atan2(n.dot(a.cross(b)), a.dot(b)) * 180.0 / std::numbers::pi;
    if (std::abs(angle) < 1e-4) {
        return;
    }
    mpr_->rotateOthers(ms->orientation(), angle);
}

void ViewerGrid::onMprSlabDragged(Viewport* vp, int plane, double thicknessMm) {
    auto* ms = asMpr(vp);
    if (ms == nullptr || ms->session() != mpr_ || !mpr_ || plane < 0 || plane > 2) {
        return;
    }
    // Half-millimetre steps; below 1 mm it is a thin plane again.
    double t = std::round(thicknessMm * 2.0) / 2.0;
    if (t < 1.0) {
        t = 0.0;
    }
    mpr_->setSlabThickness(static_cast<MprOrientation>(plane), std::min(t, 500.0));
}

void ViewerGrid::onMprCenterChanged() {
    if (!mpr_) {
        return;
    }
    busy_ = true;
    for (auto* vp : visibleViewports()) {
        if (auto* ms = asMpr(vp); ms != nullptr && ms->session() == mpr_) {
            vp->setSliceIndex(mpr_->view(ms->orientation()).sliceIndexOf(mpr_->center()));
        }
    }
    busy_ = false;
    updateCrosshairs();
    updateReferenceLines();
    if (active_ != nullptr) {
        Q_EMIT activeStateChanged(active_);
    }
}

void ViewerGrid::updateCrosshairs() {
    for (auto* vp : viewports_) {
        if (auto* ms = asMpr(vp)) {
            vp->setMprGuides(ms->guides(vp->sliceIndex()), ms->crosshairPixel(vp->sliceIndex()));
        } else {
            vp->setMprGuides({}, std::nullopt);
        }
    }
}

}  // namespace vtc
