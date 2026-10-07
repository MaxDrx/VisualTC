// Integration tests of the Qt layer, run headless (QT_QPA_PLATFORM=offscreen).
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileOpenEvent>
#include <QLabel>
#include <QMenuBar>
#include <QToolBar>
#include <QMenu>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
#include <cmath>
#include <numbers>
#include <thread>

#include "app/AppSettings.h"
#include "app/I18n.h"
#include "app/Theme.h"
#include "core/PathUtil.h"
#include "dicom/DicomParser.h"
#include "dicom/DicomStudy.h"
#include "fixtures/DicomFixtures.h"
#include "io/DecoderClient.h"
#include "io/FrameProvider.h"
#include "io/ExtractionArea.h"
#include "io/ImportTask.h"
#include "ui/MainWindow.h"
#include "ui/PreferencesDialog.h"
#include "ui/SeriesBrowser.h"
#include "ui/SeriesDock.h"
#include "measurements/MeasurementMath.h"
#include "mpr/ImageVolume.h"
#include "mpr/MprSession.h"
#include "mpr/MprSource.h"
#include "ui/ViewerGrid.h"
#include "viewer2d/AnnotationStore.h"
#include "viewer2d/StackSource.h"
#include "viewer2d/Viewport.h"

using namespace vtc;
using namespace vtc::test;

namespace {

// Series A: 16 slices 64x64, 0.5 mm pixels, 2 mm apart (z = 0..30).
// Series B: 8 slices 32x32, 1 mm pixels, 4 mm apart (z = 0..28). Same FoR.
// HU(x, y, z) = 10 * column - 1000 (series A), constant 50 (series B).
void writeSeries(const QString& dir) {
    const std::string study = makeUid("uistudy");
    const std::string forUid = makeUid("uifor");
    const std::string serA = makeUid("uiA");
    const std::string serB = makeUid("uiB");
    for (int k = 0; k < 16; ++k) {
        SyntheticImage img;
        img.studyInstanceUid = study;
        img.seriesInstanceUid = serA;
        img.frameOfReferenceUid = forUid;
        img.seriesNumber = 2;
        img.seriesDescription = "A";
        img.rows = img.columns = 64;
        img.spacingRow = img.spacingColumn = 0.5;
        img.position = {-16.0, -16.0, k * 2.0};
        img.instanceNumber = k + 1;
        img.slope = 1.0;
        img.intercept = -1024.0;
        img.windowCenter = 40;
        img.windowWidth = 400;
        img.sliceThickness = 2.0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                img.pixels.push_back(static_cast<std::uint16_t>(static_cast<std::int16_t>(10 * x - 1000 + 1024)));
            }
        }
        QVERIFY(writeDicom(utf8ToPath((dir + "/A" + QString::number(k)).toStdString()), img));
    }
    for (int k = 0; k < 8; ++k) {
        SyntheticImage img;
        img.studyInstanceUid = study;
        img.seriesInstanceUid = serB;
        img.frameOfReferenceUid = forUid;
        img.seriesNumber = 3;
        img.seriesDescription = "B";
        img.rows = img.columns = 32;
        img.spacingRow = img.spacingColumn = 1.0;
        img.position = {-16.0, -16.0, k * 4.0};
        img.instanceNumber = k + 1;
        img.slope = 1.0;
        img.intercept = -1024.0;
        img.pixels.assign(32 * 32, static_cast<std::uint16_t>(50 + 1024));
        QVERIFY(writeDicom(utf8ToPath((dir + "/B" + QString::number(k)).toStdString()), img, Encoding::JpegLsLossless));
    }
}

}  // namespace

class UiTests : public QObject {
    Q_OBJECT

private:
    QTemporaryDir tmp_;
    StudyDatabase db_;
    FrameProvider* frames_ = nullptr;
    AnnotationStore* store_ = nullptr;
    SeriesPtr seriesA_;
    SeriesPtr seriesB_;

    void waitLoaded(Viewport& vp) { QTRY_VERIFY_WITH_TIMEOUT(vp.currentFrame() != nullptr, 20000); }

    std::shared_ptr<MprSession> buildMprSession() {
        auto fetch = [this](const FrameRef& r) { return frames_->decodeNow(r); };
        auto built = ImageVolume::build(*seriesA_, fetch, 1ull << 30);
        return built.volume ? std::make_shared<MprSession>(built.volume, seriesA_) : nullptr;
    }

    // Optional pictures for a visual check: VISUALTC_TEST_SCREENSHOTS=<folder>.
    static void saveShot(QWidget* w, const QString& name) {
        const QString dir = qEnvironmentVariable("VISUALTC_TEST_SCREENSHOTS");
        if (!dir.isEmpty()) {
            w->grab().save(dir + "/" + name + ".png");
        }
    }

    static size_t guideIndex(const Viewport& vp, MprOrientation plane) {
        for (size_t i = 0; i < vp.mprGuides().size(); ++i) {
            if (vp.mprGuides()[i].plane == static_cast<int>(plane)) {
                return i;
            }
        }
        return vp.mprGuides().size();
    }

    static double distanceToLine(const QPointF& p, const Viewport::GuideHandles& h) {
        const QPointF rel = p - h.center;
        return std::abs(rel.x() * h.dir.y() - rel.y() * h.dir.x());
    }

    // An axial series and its scout (coronal topogram) of the same exam; the
    // scout has its own Frame of Reference, as some scanners write.
    static bool writeScoutExam(const QString& path) {
        bool ok = true;
        const std::string study = makeUid("refstudy");
        const std::string axialUid = makeUid("refaxial");
        const std::string scoutUid = makeUid("refscout");
        const std::string axialFor = makeUid("refforA");
        for (int k = 0; k < 6; ++k) {
            SyntheticImage img;
            img.studyInstanceUid = study;
            img.seriesInstanceUid = axialUid;
            img.frameOfReferenceUid = axialFor;
            img.seriesNumber = 2;
            img.seriesDescription = "AXIAL";
            img.rows = img.columns = 32;
            img.spacingRow = img.spacingColumn = 1.0;
            img.position = {-16.0, -16.0, 10.0 + 2.0 * k};
            img.instanceNumber = k + 1;
            img.pixels.assign(32 * 32, 1024);
            ok = ok && writeDicom(utf8ToPath((path + "/AX" + QString::number(k)).toStdString()), img);
        }
        SyntheticImage scout;
        scout.studyInstanceUid = study;
        scout.seriesInstanceUid = scoutUid;
        scout.frameOfReferenceUid = makeUid("refforS");
        scout.seriesNumber = 1;
        scout.seriesDescription = "TOPOGRAMA";
        scout.rows = 48;
        scout.columns = 32;
        scout.spacingRow = scout.spacingColumn = 1.0;
        scout.rowDir = {1, 0, 0};
        scout.colDir = {0, 0, -1};
        scout.position = {-16.0, 0.0, 40.0};
        scout.instanceNumber = 1;
        scout.pixels.assign(48 * 32, 1024);
        ok = ok && writeDicom(utf8ToPath((path + "/SCOUT").toStdString()), scout);
        return ok;
    }

    static QAction* actionWithShortcut(QWidget* window, const QKeySequence& ks) {
        for (auto* a : window->findChildren<QAction*>()) {
            if (a->shortcut() == ks) {
                return a;
            }
        }
        return nullptr;
    }

    // Main window with an exam imported; returns the viewport showing it.
    // On failure, `why` tells what the window was doing (slow CI machines).
    Viewport* openWindowWith(MainWindow* window, const QString& path, QString* why = nullptr) {
        window->resize(1200, 800);
        window->show();
        window->importPaths({path});
        Viewport* active = nullptr;
        const bool ok = QTest::qWaitFor(
            [&] {
                for (auto* vp : window->findChildren<Viewport*>()) {
                    if (vp->isVisible() && vp->currentFrame()) {
                        active = vp;
                        return true;
                    }
                }
                return false;
            },
            90000);
        QCoreApplication::processEvents();
        if (!ok && why != nullptr) {
            const auto* browser = window->findChild<SeriesBrowser*>();
            *why = QStringLiteral("series=%1").arg(browser != nullptr ? browser->seriesCount() : -1);
            for (auto* label : window->findChildren<QLabel*>()) {
                if (!label->text().isEmpty()) {
                    *why += " | " + label->text();
                }
            }
            for (auto* vp : window->findChildren<Viewport*>()) {
                if (vp->isVisible()) {
                    *why += QStringLiteral(" | viewport: source=%1 error=%2")
                                .arg(vp->source() != nullptr)
                                .arg(vp->errorText());
                }
            }
        }
        return ok ? active : nullptr;
    }

private Q_SLOTS:
    void initTestCase() {
        // QSettings needs an organization/application name to store anything
        // (on Windows and macOS it silently refuses otherwise). A separate
        // name keeps the tests away from the real VisualTC preferences.
        QCoreApplication::setOrganizationName(QStringLiteral("VisualTC-Testes"));
        QCoreApplication::setApplicationName(QStringLiteral("visualtc_ui_tests"));
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(tmp_.isValid());
        writeSeries(tmp_.path());
        DecoderClient::initialize(true, QStringLiteral(VISUALTC_WORKER_PATH));
        QVERIFY(DecoderClient::isolationActive());
        frames_ = new FrameProvider(256ull << 20, 2, this);
        store_ = new AnnotationStore(this);

        ImportTask task;
        QSignalSpy done(&task, &ImportTask::finished);
        QVERIFY(task.start({tmp_.path()}));
        QVERIFY(done.wait(30000));
        QCOMPARE(task.result().instances.size(), std::size_t(24));
        QCOMPARE(task.result().issues.size(), std::size_t(0));
        db_.addInstances(task.result().instances);
        for (const auto& s : db_.allSeries()) {
            (s->frameCount() == 16 ? seriesA_ : seriesB_) = s;
        }
        QVERIFY(seriesA_ && seriesB_);
        QVERIFY(seriesA_->geometry.volumetric);
    }

    void displaysImageWithDicomWindow() {
        Viewport vp(store_);
        vp.resize(400, 400);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(seriesA_, frames_, false));
        waitLoaded(vp);
        QCOMPARE(vp.windowCenter(), 40.0);
        QCOMPARE(vp.windowWidth(), 400.0);
        QCOMPARE(vp.currentFrame()->valueAt(10, 3), 10.0 * 10 - 1000);
        const QImage img = vp.renderImage(true, true);
        QVERIFY(!img.isNull());
    }

    void windowLevelDragFollowsMouse() {
        Viewport vp(store_);
        vp.resize(400, 400);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(seriesA_, frames_, false));
        waitLoaded(vp);
        vp.setTool(Tool::WindowLevel);
        QTest::mousePress(&vp, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
        QTest::mouseMove(&vp, QPoint(250, 210));
        QTest::mouseRelease(&vp, Qt::LeftButton, Qt::NoModifier, QPoint(250, 210));
        // sensitivity = width / 250 = 1.6 per pixel
        QCOMPARE(vp.windowWidth(), 400.0 + 50 * 1.6);
        QCOMPARE(vp.windowCenter(), 40.0 + 10 * 1.6);
    }

    void wheelAndKeyboardNavigation() {
        Viewport vp(store_);
        vp.resize(400, 400);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(seriesA_, frames_, false));
        waitLoaded(vp);
        QCOMPARE(vp.sliceIndex(), 0);
        QWheelEvent wheel(QPointF(200, 200), vp.mapToGlobal(QPointF(200, 200)), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&vp, &wheel);
        QCOMPARE(vp.sliceIndex(), 1);
        QTest::keyClick(&vp, Qt::Key_End);
        QCOMPARE(vp.sliceIndex(), 15);
        QTest::keyClick(&vp, Qt::Key_Up);
        QCOMPARE(vp.sliceIndex(), 14);
        QTest::keyClick(&vp, Qt::Key_Home);
        QCOMPARE(vp.sliceIndex(), 0);
    }

    void distanceMeasurementInMillimetres() {
        Viewport vp(store_);
        vp.resize(400, 400);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(seriesA_, frames_, false));
        waitLoaded(vp);
        vp.setTool(Tool::Distance);
        QTest::mousePress(&vp, Qt::LeftButton, Qt::NoModifier, QPoint(100, 200));
        QTest::mouseMove(&vp, QPoint(200, 200));
        QTest::mouseRelease(&vp, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
        const auto& list = store_->list(vp.source()->annotationKey(vp.sliceIndex()));
        QCOMPARE(list.size(), std::size_t(1));
        const auto& a = list.front();
        // Fit zoom: 0.98 * 400 / (64 * 0.5) = 12.25 screen px per mm -> 100 px = 8.163 mm
        const double mm = distanceMm(a->points[0], a->points[1], 0.5, 0.5);
        QVERIFY(std::abs(mm - 100.0 / 12.25) < 1e-6);
        QVERIFY(a->labelLines(vp.measureContext()).front().contains("mm"));

        // Undo / redo / delete
        store_->undoStack()->undo();
        QCOMPARE(store_->list(vp.source()->annotationKey(0)).size(), std::size_t(0));
        store_->undoStack()->redo();
        QCOMPARE(store_->list(vp.source()->annotationKey(0)).size(), std::size_t(1));
        store_->clearAll();
        QCOMPARE(store_->totalCount(), 0);
    }

    void roiReportsHounsfieldUnits() {
        Viewport vp(store_);
        vp.resize(400, 400);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(seriesA_, frames_, false));
        waitLoaded(vp);
        vp.setTool(Tool::RectRoi);
        QTest::mousePress(&vp, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        QTest::mouseMove(&vp, QPoint(250, 250));
        QTest::mouseRelease(&vp, Qt::LeftButton, Qt::NoModifier, QPoint(250, 250));
        const auto& list = store_->list(vp.source()->annotationKey(vp.sliceIndex()));
        QCOMPARE(list.size(), std::size_t(1));
        const QStringList lines = list.front()->labelLines(vp.measureContext());
        // Symmetric ROI around the image centre: mean HU = 10 * 31.5 - 1000 = -685
        bool found = false;
        for (const auto& l : lines) {
            if (l.startsWith(QStringLiteral("Média"))) {
                found = l.contains("HU");
            }
        }
        QVERIFY(found);
        const auto values = list.front()->roiValues(vp.measureContext());
        double mean = 0;
        for (double v : values) {
            mean += v;
        }
        mean /= static_cast<double>(values.size());
        QVERIFY(std::abs(mean - (-685.0)) < 10.5);
        store_->clearAll();
    }

    void spatialSyncUsesPatientPosition() {
        ViewerGrid grid(store_);
        grid.resize(800, 400);
        grid.show();
        grid.setLayoutGrid(1, 2);
        auto vps = grid.visibleViewports();
        grid.showSource(vps[0], std::make_shared<StackSource>(seriesA_, frames_, false));
        grid.showSource(vps[1], std::make_shared<StackSource>(seriesB_, frames_, false));
        waitLoaded(*vps[0]);
        waitLoaded(*vps[1]);
        grid.setSyncEnabled(true);
        QVERIFY(grid.syncProblem().isEmpty());
        vps[0]->setSliceIndex(4);  // z = 8 mm in A
        QCOMPARE(vps[1]->sliceIndex(), 2);  // z = 8 mm in B (not index 4!)
        vps[0]->setSliceIndex(13);  // z = 26 -> B nearest z = 24 or 28
        QVERIFY(vps[1]->sliceIndex() == 6 || vps[1]->sliceIndex() == 7);
        vps[1]->setSliceIndex(1);  // z = 4 in B -> A z = 4 (index 2)
        QCOMPARE(vps[0]->sliceIndex(), 2);
    }

    void mprCrosshairDrivesOtherPlanes() {
        auto fetch = [this](const FrameRef& r) { return frames_->decodeNow(r); };
        auto built = ImageVolume::build(*seriesA_, fetch, 1ull << 30);
        QVERIFY2(built.volume != nullptr, built.error.c_str());
        auto session = std::make_shared<MprSession>(built.volume, seriesA_);
        ViewerGrid grid(store_);
        grid.resize(1200, 400);
        grid.show();
        grid.enterMpr(session);
        auto vps = grid.visibleViewports();
        QCOMPARE(vps.size(), std::size_t(3));
        for (auto* vp : vps) {
            waitLoaded(*vp);
        }
        // Click in the axial view: crosshair moves to that point.
        Viewport* axial = vps[0];
        const QPoint click(140, 120);
        QTest::mousePress(axial, Qt::LeftButton, Qt::NoModifier, click);
        QTest::mouseRelease(axial, Qt::LeftButton, Qt::NoModifier, click);
        const auto picked = axial->patientPointAt(click);
        QVERIFY(picked.has_value());
        QVERIFY(distance(session->center(), *picked) < 1e-6);
        const auto* coronal = dynamic_cast<MprSource*>(vps[1]->source().get());
        const auto* sagittal = dynamic_cast<MprSource*>(vps[2]->source().get());
        QCOMPARE(vps[1]->sliceIndex(), session->view(coronal->orientation()).sliceIndexOf(session->center()));
        QCOMPARE(vps[2]->sliceIndex(), session->view(sagittal->orientation()).sliceIndexOf(session->center()));
        // The coronal plane now passes through the clicked y coordinate.
        const auto plane = coronal->planeAt(vps[1]->sliceIndex());
        QVERIFY(std::abs(plane.origin.y - picked->y) <= session->view(MprOrientation::Coronal).step / 2 + 1e-6);
        // Scrolling the axial view moves the crosshair along z.
        const double z0 = session->center().z;
        axial->scrollBy(3);
        QVERIFY(std::abs(std::abs(session->center().z - z0) - 6.0) < 1e-6);
        grid.exitMpr();
    }

    void mprGuideLinesMoveTurnAndThicken() {
        auto session = buildMprSession();
        QVERIFY(session != nullptr);
        ViewerGrid grid(store_);
        grid.resize(1200, 400);
        grid.show();
        grid.enterMpr(session);
        auto vps = grid.visibleViewports();
        QCOMPARE(vps.size(), std::size_t(3));
        for (auto* vp : vps) {
            waitLoaded(*vp);
        }
        Viewport* axial = vps[0];
        QCOMPARE(axial->mprGuides().size(), std::size_t(2));
        const size_t gi = guideIndex(*axial, MprOrientation::Coronal);
        QVERIFY(gi < 2);
        auto h = axial->guideHandles(gi);
        QVERIFY(h.has_value());
        QVERIFY(h->pxPerMm > 1.0);

        // 1. Drag the coronal line itself: only the coronal plane moves.
        const Vec3 c0 = session->center();
        const int ax0 = vps[0]->sliceIndex();
        const int cor0 = vps[1]->sliceIndex();
        const int sag0 = vps[2]->sliceIndex();
        const QPoint grab = (h->center + h->dir * 50.0).toPoint();
        const QPoint drop = (QPointF(grab) + h->normal * 40.0).toPoint();
        QTest::mouseMove(axial, grab);
        const Qt::CursorShape lineCursor =
            std::abs(h->normal.y()) > std::abs(h->normal.x()) ? Qt::SplitVCursor : Qt::SplitHCursor;
        QCOMPARE(axial->cursor().shape(), lineCursor);
        QTest::mousePress(axial, Qt::LeftButton, Qt::NoModifier, grab);
        QTest::mouseMove(axial, drop);
        QTest::mouseRelease(axial, Qt::LeftButton, Qt::NoModifier, drop);
        const Vec3 c1 = session->center();
        const double moved = (c1 - c0).dot(session->view(MprOrientation::Coronal).n);
        QVERIFY2(std::abs(std::abs(moved) - 40.0 / h->pxPerMm) < 0.2, qPrintable(QString::number(moved)));
        QVERIFY(std::abs((c1 - c0).dot(session->view(MprOrientation::Sagittal).n)) < 1e-6);
        QVERIFY(std::abs((c1 - c0).dot(session->view(MprOrientation::Axial).n)) < 1e-6);
        QCOMPARE(vps[0]->sliceIndex(), ax0);
        QCOMPARE(vps[2]->sliceIndex(), sag0);
        QVERIFY(vps[1]->sliceIndex() != cor0);
        h = axial->guideHandles(gi);
        QVERIFY(distanceToLine(drop, *h) < 1.5);  // the line followed the mouse

        // 2. Drag the round handle around the centre: the coronal and sagittal
        // planes turn together (oblique MPR) and the line follows the mouse.
        const Vec3 nAxial = session->view(MprOrientation::Axial).n;
        const Vec3 nCor0 = session->view(MprOrientation::Coronal).n;
        const double zoomCoronal = vps[1]->zoomPercent();
        const QPointF centre = h->center;
        const QPointF start = h->rotate[0];
        const double radius = std::hypot(start.x() - centre.x(), start.y() - centre.y());
        QTest::mouseMove(axial, start.toPoint());
        QCOMPARE(axial->cursor().shape(), Qt::BitmapCursor);  // the "turn" cursor
        saveShot(&grid, QStringLiteral("mpr-rotate-hover"));
        QTest::mousePress(axial, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        QPointF last = start;
        for (int k = 1; k <= 6; ++k) {
            const double a = k * 5.0 * std::numbers::pi / 180.0;
            const QPointF d0 = (start - centre) / radius;
            last = centre + QPointF(d0.x() * std::cos(a) - d0.y() * std::sin(a),
                                    d0.x() * std::sin(a) + d0.y() * std::cos(a)) * radius;
            QTest::mouseMove(axial, last.toPoint());
        }
        QTest::mouseRelease(axial, Qt::LeftButton, Qt::NoModifier, last.toPoint());
        QVERIFY(session->isOblique(MprOrientation::Coronal));
        QVERIFY(session->isOblique(MprOrientation::Sagittal));
        QVERIFY(!session->isOblique(MprOrientation::Axial));
        const Vec3 nCor = session->view(MprOrientation::Coronal).n;
        const Vec3 nSag = session->view(MprOrientation::Sagittal).n;
        QVERIFY(std::abs(nCor.dot(nSag)) < 1e-9);
        QVERIFY(std::abs(nCor.dot(nAxial)) < 1e-9);
        QVERIFY(std::abs(nSag.dot(nAxial)) < 1e-9);
        const double turned = std::acos(std::clamp(nCor.dot(nCor0), -1.0, 1.0)) * 180.0 / std::numbers::pi;
        QVERIFY2(std::abs(turned - 30.0) < 1.0, qPrintable(QString::number(turned)));
        h = axial->guideHandles(gi);
        QVERIFY(distanceToLine(last, *h) < 2.0);
        QCOMPARE(vps[1]->zoomPercent(), zoomCoronal);  // the turning plane keeps its scale
        QVERIFY(vps[1]->source()->seriesLabel().contains(QStringLiteral("oblíquo")));
        QVERIFY(!vps[0]->source()->seriesLabel().contains(QStringLiteral("oblíquo")));
        saveShot(&grid, QStringLiteral("mpr-rotated"));

        // 3. Pull a slab bar away from the line: MIP thickness of that plane only.
        const int vAx = session->version(MprOrientation::Axial);
        const int vSag = session->version(MprOrientation::Sagittal);
        const int vCor = session->version(MprOrientation::Coronal);
        const QPointF bar = h->slab[0];
        QTest::mousePress(axial, Qt::LeftButton, Qt::NoModifier, bar.toPoint());
        QTest::mouseMove(axial, (bar + h->normal * 30.0).toPoint());
        QTest::mouseRelease(axial, Qt::LeftButton, Qt::NoModifier, (bar + h->normal * 30.0).toPoint());
        const double t = session->slab(MprOrientation::Coronal).thickness;
        QVERIFY2(std::abs(t - 60.0 / h->pxPerMm) <= 0.5, qPrintable(QString::number(t)));
        QCOMPARE(session->slab(MprOrientation::Coronal).mode, SlabMode::MIP);
        QCOMPARE(session->slab(MprOrientation::Axial).thickness, 0.0);
        QCOMPARE(session->slab(MprOrientation::Sagittal).thickness, 0.0);
        QCOMPARE(session->version(MprOrientation::Axial), vAx);  // not recomputed
        QCOMPARE(session->version(MprOrientation::Sagittal), vSag);
        QVERIFY(session->version(MprOrientation::Coronal) != vCor);
        QVERIFY(vps[1]->source()->seriesLabel().contains(QStringLiteral("MIP")));
        QCOMPARE(axial->mprGuides()[gi].thickness, t);
        QTest::mouseMove(axial, axial->guideHandles(gi)->slab[0].toPoint());
        saveShot(&grid, QStringLiteral("mpr-slab-hover"));
        // Pushed back across the line: a thin plane again.
        h = axial->guideHandles(gi);
        const QPointF bar2 = h->slab[0];
        const QPointF across = h->center + h->dir * 60.0 - h->normal * 6.0;
        QTest::mousePress(axial, Qt::LeftButton, Qt::NoModifier, bar2.toPoint());
        QTest::mouseMove(axial, across.toPoint());
        QTest::mouseRelease(axial, Qt::LeftButton, Qt::NoModifier, across.toPoint());
        QCOMPARE(session->slab(MprOrientation::Coronal).thickness, 0.0);

        // 4. The centre moves both lines at once.
        h = axial->guideHandles(gi);
        const QPoint from = h->center.toPoint();
        const QPoint to = from + QPoint(25, -15);
        QTest::mouseMove(axial, from);
        QCOMPARE(axial->cursor().shape(), Qt::SizeAllCursor);
        QTest::mousePress(axial, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(axial, to);
        QTest::mouseRelease(axial, Qt::LeftButton, Qt::NoModifier, to);
        const auto under = axial->patientPointAt(QPointF(to) - (QPointF(from) - h->center));
        QVERIFY(under.has_value());
        QVERIFY(distance(session->center(), *under) < 0.2);

        // Handles are on screen only: exported images keep just the lines.
        QVERIFY(!axial->renderImage(true, true).isNull());
        grid.exitMpr();
    }

    void mprSessionKeepsPlanesIndependent() {
        auto session = buildMprSession();
        QVERIFY(session != nullptr);
        const int vAx = session->version(MprOrientation::Axial);
        const int vCor = session->version(MprOrientation::Coronal);
        session->setSlabThickness(MprOrientation::Coronal, 8.0);
        QCOMPARE(session->slab(MprOrientation::Coronal).thickness, 8.0);
        QCOMPARE(session->slab(MprOrientation::Axial).thickness, 0.0);
        QCOMPARE(session->version(MprOrientation::Axial), vAx);
        QVERIFY(session->version(MprOrientation::Coronal) != vCor);
        // A new projection mode recomputes only the thick planes.
        const int vCor2 = session->version(MprOrientation::Coronal);
        session->setSlabMode(SlabMode::MinIP);
        QCOMPARE(session->slab(MprOrientation::Coronal).mode, SlabMode::MinIP);
        QVERIFY(session->version(MprOrientation::Coronal) != vCor2);
        QCOMPARE(session->version(MprOrientation::Axial), vAx);
        // The menu sets the three planes at once; limits are respected.
        session->setSlab({3.0, SlabMode::Average});
        for (auto o : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
            QCOMPARE(session->slab(o).thickness, 3.0);
            QCOMPARE(session->slab(o).mode, SlabMode::Average);
        }
        session->setSlabThickness(MprOrientation::Axial, 9000.0);
        QCOMPARE(session->slab(MprOrientation::Axial).thickness, 500.0);
        session->setSlabThickness(MprOrientation::Axial, -4.0);
        QCOMPARE(session->slab(MprOrientation::Axial).thickness, 0.0);

        // Turning keeps the three planes perpendicular, even after many small
        // steps, and the crosshair stays inside the (unrotated) volume box.
        for (int i = 0; i < 360; ++i) {
            session->rotateOthers(MprOrientation::Axial, 0.5);
        }
        for (int i = 0; i < 100; ++i) {
            session->rotateOthers(MprOrientation::Coronal, -0.7);
        }
        const Vec3 a = session->view(MprOrientation::Axial).n;
        const Vec3 c = session->view(MprOrientation::Coronal).n;
        const Vec3 s = session->view(MprOrientation::Sagittal).n;
        QVERIFY(std::abs(a.dot(c)) < 1e-9 && std::abs(a.dot(s)) < 1e-9 && std::abs(c.dot(s)) < 1e-9);
        QVERIFY(std::abs(a.norm() - 1.0) < 1e-9 && std::abs(c.norm() - 1.0) < 1e-9);
        QVERIFY(session->isOblique());
        session->setCenter({1000.0, -1000.0, 1000.0});
        const MprView& box = session->orthogonalView(MprOrientation::Axial);
        const Vec3 p = session->center();
        QVERIFY(p.dot(box.u) <= box.uMax + 1e-9 && p.dot(box.u) >= box.uMin - 1e-9);
        QVERIFY(p.dot(box.n) <= box.nMax + 1e-9 && p.dot(box.n) >= box.nMin - 1e-9);
        // Straight planes again.
        session->resetOrientation();
        QVERIFY(!session->isOblique());
        QVERIFY(std::abs(session->view(MprOrientation::Coronal).n.dot(box.n)) < 1e-12);

        // Each plane draws the two others, with its slab boundary 1 mm away
        // per millimetre of half-thickness.
        MprSource axial(session, MprOrientation::Axial);
        const auto guides = axial.guides(axial.initialIndex());
        QCOMPARE(guides.size(), std::size_t(2));
        for (const auto& g : guides) {
            QVERIFY(g.plane != static_cast<int>(MprOrientation::Axial));
            const MprView& v = session->view(MprOrientation::Axial);
            const double mm = std::hypot(g.mmOffset.x * v.spacingU, g.mmOffset.y * v.spacingV);
            QVERIFY(std::abs(mm - 1.0) < 1e-9);  // straight planes: exactly 1 mm across
            QCOMPARE(g.thickness, 3.0);
        }
    }

    void colorMapsTintGreyscaleImages() {
        Viewport vp(store_);
        vp.resize(400, 400);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(seriesA_, frames_, false));
        waitLoaded(vp);
        // Columns 0..63 hold -1000..-370 HU: a ramp across this window.
        vp.setWindow(-685.0, 640.0);
        const QColor grey = vp.renderImage(false, false).pixelColor(200, 200);
        QCOMPARE(grey.red(), grey.green());
        QCOMPARE(grey.green(), grey.blue());
        vp.setColorMap(ColorMap::HotIron);
        QCOMPARE(vp.colorMap(), ColorMap::HotIron);
        const QColor hot = vp.renderImage(false, false).pixelColor(200, 200);  // middle grey -> red
        QVERIFY2(hot.red() > 180 && hot.green() < 80 && hot.blue() < 40, qPrintable(hot.name()));
        const QColor dark = vp.renderImage(false, false).pixelColor(30, 200);  // left: below the window
        QVERIFY(dark.red() < 40 && dark.green() < 40 && dark.blue() < 40);
        // Display only: the pixel values (HU) and the measurements do not change.
        QCOMPARE(vp.currentFrame()->valueAt(10, 3), 10.0 * 10 - 1000);
        QCOMPARE(vp.viewState().colorMap, ColorMap::HotIron);
        vp.setColorMap(ColorMap::Gray);
        const QColor back = vp.renderImage(false, false).pixelColor(200, 200);
        QCOMPARE(back, grey);
    }

    void roiHistogramFollowsTheSelectedRoi() {
        QStandardPaths::setTestModeEnabled(true);
        auto* window = new MainWindow;
        QString why;
        Viewport* vp = openWindowWith(window, tmp_.path(), &why);
        QVERIFY2(vp != nullptr, qPrintable(why));
        QAction* hist = actionWithShortcut(window, QKeySequence(QStringLiteral("Ctrl+Shift+H")));
        QVERIFY(hist != nullptr);
        QVERIFY(!hist->isEnabled());  // nothing selected yet
        vp->setTool(Tool::RectRoi);
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));
        QTest::mouseMove(vp, QPoint(260, 240));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, QPoint(260, 240));
        QVERIFY(vp->selectedAnnotation() != nullptr);
        QVERIFY(hist->isEnabled());  // the new ROI is selected
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, QPoint(15, 15));
        QVERIFY(vp->selectedAnnotation() == nullptr);
        QVERIFY(!hist->isEnabled());
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, QPoint(150, 150));  // its corner
        QVERIFY(vp->selectedAnnotation() != nullptr);
        QVERIFY(hist->isEnabled());
        QString shown;
        QTimer::singleShot(100, window, [&shown] {
            if (auto* w = QApplication::activeModalWidget()) {
                shown = QString::fromLatin1(w->metaObject()->className());
                w->close();
            }
        });
        hist->trigger();
        QCOMPARE(shown, QStringLiteral("vtc::HistogramDialog"));
        store_->clearAll();
        delete window;
    }

    void referenceLinesOpenTheScoutBesideASingleView() {
        // An axial series and its scout (coronal topogram) of the same exam;
        // the scout has its own Frame of Reference, as some scanners write.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeScoutExam(dir.path()));

        QStandardPaths::setTestModeEnabled(true);
        auto* window = new MainWindow;
        QString why;
        QVERIFY2(openWindowWith(window, dir.path(), &why) != nullptr, qPrintable(why));
        auto* grid = window->findChild<ViewerGrid*>();
        QVERIFY(grid != nullptr);
        grid->setLayoutGrid(1, 1);
        QVERIFY(grid->activeViewport()->currentFrame() != nullptr);
        QCOMPARE(grid->referenceLineCount(), 0);
        QAction* ref = actionWithShortcut(window, QKeySequence(QStringLiteral("Ctrl+L")));
        QVERIFY(ref != nullptr && ref->isCheckable());
        if (ref->isChecked()) {
            ref->trigger();  // off
        }
        ref->trigger();  // on: with one image on screen, the other plane opens beside it
        QVERIFY(ref->isChecked());
        QCOMPARE(grid->visibleViewports().size(), std::size_t(2));
        QTRY_VERIFY_WITH_TIMEOUT(grid->referenceLineCount() > 0, 20000);
        saveShot(window, QStringLiteral("reference-lines"));
        delete window;
    }

    void isolatedDecoderSurvivesCrash() {
        const std::string path = seriesA_->frames.front().instance->filePath;
        QVERIFY(DecoderClient::decode(path).ok());
        // A worker that ended while idle (killed by the system, a computer
        // that slept) is replaced without failing the next image. Decoding
        // threads have no event loop: nothing tells QProcess it ended.
        std::string silentEndError = "not run";
        std::thread decoderThread([&path, &silentEndError] {
            if (!DecoderClient::decode(path).ok()) {
                silentEndError = "first decode failed";
                return;
            }
            DecoderClient::endWorkerSilentlyForTest();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const auto again = DecoderClient::decode(path);
            silentEndError = again.ok() ? std::string() : again.error;
            DecoderClient::releaseThreadWorker();
        });
        decoderThread.join();
        QVERIFY2(silentEndError.empty(), silentEndError.c_str());
        QVERIFY(DecoderClient::crashWorkerForTest());
        // The next request transparently starts a fresh worker.
        const auto again = DecoderClient::decode(path);
        QVERIFY(again.ok());
        // A corrupt file is reported, not fatal.
        const QString bad = tmp_.path() + "/corrompido";
        {
            QFile f(bad);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QFile src(QString::fromStdString(path));
            QVERIFY(src.open(QIODevice::ReadOnly));
            QByteArray bytes = src.readAll();
            bytes.truncate(bytes.size() / 3);
            f.write(bytes);
        }
        const auto broken = DecoderClient::decode(bad.toStdString());
        QVERIFY(!broken.ok());
        QVERIFY(!broken.error.empty());
        DecoderClient::releaseThreadWorker();
    }

    void failedDecodeIsNotRetriedInALoop() {
        // A file that was readable at scan time but fails to decode (here it
        // is truncated afterwards) must be decoded once and then reported,
        // not re-requested on every repaint.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::vector<InstancePtr> insts;
        const std::string seriesUid = makeUid("broken");
        for (int k = 0; k < 3; ++k) {
            SyntheticImage img;
            img.rows = img.columns = 32;
            img.position = {0.0, 0.0, k * 2.0};
            img.instanceNumber = k + 1;
            img.seriesInstanceUid = seriesUid;
            img.pixels.assign(32 * 32, 1024);
            const auto path = utf8ToPath((dir.path() + "/S" + QString::number(k)).toStdString());
            QVERIFY(writeDicom(path, img));
            const auto parsed = parseDicomHeader(path);
            QVERIFY(parsed.status == ParseStatus::Ok);
            insts.push_back(parsed.instance);
        }
        StudyDatabase db;
        db.addInstances(insts);
        const SeriesPtr series = db.allSeries().front();
        {
            QFile f(QString::fromStdString(series->frames.front().instance->filePath));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.resize(f.size() / 2));
        }
        FrameProvider provider(64ull << 20, 2);
        Viewport vp(store_);
        vp.resize(300, 300);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(series, &provider, false));
        QTRY_VERIFY_WITH_TIMEOUT(!vp.errorText().isEmpty(), 20000);
        // The prefetch of the neighbours may still be finishing (slow under
        // sanitizers): count once the queue is empty, then it must not grow.
        QTRY_COMPARE_WITH_TIMEOUT(provider.pendingCount(), 0, 20000);
        const int afterError = provider.decodeCount();
        QTest::qWait(800);
        QCoreApplication::processEvents();
        QCOMPARE(provider.decodeCount(), afterError);
        QVERIFY(afterError <= 3);  // the broken slice plus at most its two neighbours
        provider.shutdown();
    }

    void multiFrameLargerThanCacheDoesNotThrash() {
        // Enhanced CT whose decoded frames exceed the cache budget: showing a
        // frame must decode the file once, and nearby frames must stay cached.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        EnhancedSpec spec;
        spec.rows = spec.columns = 128;
        for (int k = 0; k < 48; ++k) {
            spec.origins.push_back({0.0, 0.0, k * 1.0});
            spec.hu.emplace_back(128 * 128, static_cast<std::int16_t>(k));
        }
        const auto path = utf8ToPath((dir.path() + "/enhanced").toStdString());
        QVERIFY(writeEnhancedCt(path, spec));
        const auto parsed = parseDicomHeader(path);
        QVERIFY(parsed.status == ParseStatus::Ok);
        StudyDatabase db;
        db.addInstances({parsed.instance});
        const SeriesPtr series = db.allSeries().front();
        QCOMPARE(series->frameCount(), 48);
        // 48 frames x 32 KiB = 1.5 MiB of pixels; the cache holds about a third.
        FrameProvider provider(512ull << 10, 2);
        Viewport vp(store_);
        vp.resize(300, 300);
        vp.show();
        vp.setSource(std::make_shared<StackSource>(series, &provider, false));
        vp.setSliceIndex(30);
        QTRY_VERIFY_WITH_TIMEOUT(vp.currentFrame() != nullptr && vp.sliceIndex() == 30, 20000);
        // HU of frame k is k and frame k lies at z = k mm.
        QCOMPARE(vp.currentFrame()->valueAt(5, 5), series->frames[30].geometry().position.z);
        QTest::qWait(500);
        QCoreApplication::processEvents();
        QVERIFY2(provider.decodeCount() <= 2, qPrintable(QString::number(provider.decodeCount())));
        const int before = provider.decodeCount();
        vp.scrollBy(1);
        vp.scrollBy(-2);
        QVERIFY(vp.currentFrame() != nullptr);
        QCOMPARE(provider.decodeCount(), before);  // neighbours came from the cache
        provider.shutdown();
    }

    void mainWindowShortcutsAreUniqueAndPresetsWork() {
        QStandardPaths::setTestModeEnabled(true);
        auto* window = new MainWindow;
        window->resize(1200, 800);
        window->show();
        window->importPaths({tmp_.path()});
        Viewport* active = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (auto* vp : window->findChildren<Viewport*>()) {
                    if (vp->isVisible() && vp->currentFrame()) {
                        active = vp;
                        return true;
                    }
                }
                return false;
            }(),
            30000);
        QCoreApplication::processEvents();
        // The presets menu is (re)built when opened: simulate that first, as
        // a user who looked at the menu once.
        for (auto* menu : window->findChildren<QMenu*>()) {
            if (menu->title() == QStringLiteral("Presets de janela")) {
                Q_EMIT menu->aboutToShow();
            }
        }
        // Every key sequence must belong to exactly one action in the window.
        std::map<QString, QStringList> owners;
        for (auto* a : window->findChildren<QAction*>()) {
            for (const auto& ks : a->shortcuts()) {
                if (!ks.isEmpty() && a->shortcutContext() != Qt::WidgetShortcut) {
                    owners[ks.toString()] << a->text();
                }
            }
        }
        for (const auto& [key, names] : owners) {
            QVERIFY2(names.size() == 1, qPrintable(key + ": " + names.join(" | ")));
        }
        // Key "1" applies the lung window (CT preset 1).
        active->setFocus();
        QTest::keyClick(window, Qt::Key_1);
        QTRY_COMPARE(active->windowCenter(), -600.0);
        QCOMPARE(active->windowWidth(), 1500.0);
        delete window;
    }

    void importOpensPasswordProtectedZip() {
        // A ZIP with five slices of series A, protected with AES-256; the
        // user first types a wrong password, then the right one.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        std::vector<ArchiveMember> members;
        for (int k = 0; k < 5; ++k) {
            const auto& path = seriesA_->frames[static_cast<size_t>(k)].instance->filePath;
            members.push_back({"EXAME/DICOM/IM" + std::to_string(k), readBytes(utf8ToPath(path)), {}});
        }
        const QString zip = dir.path() + "/exame do paciente.zip";
        QVERIFY(writeArchive(utf8ToPath(zip.toStdString()), ArchiveFormat::Zip, members, "senha123", "aes256"));
        ExtractionArea area(dir.path() + "/cache");
        QVERIFY(area.valid());

        ImportTask task;
        QStringList asked;
        task.setPasswordProvider([&](const QString& name, bool retry) -> std::optional<QString> {
            asked << name;
            return retry ? QStringLiteral("senha123") : QStringLiteral("errada");
        });
        QSignalSpy done(&task, &ImportTask::finished);
        QVERIFY(task.start({zip}, area.newImportDir()));
        QVERIFY(done.wait(60000));
        QCOMPARE(asked.size(), 2);
        QCOMPARE(asked.front(), QStringLiteral("exame do paciente.zip"));
        const ScanResult& r = task.result();
        QCOMPARE(r.archivesOpened, std::size_t(1));
        QCOMPARE(r.instances.size(), std::size_t(5));
        QCOMPARE(r.issues.size(), std::size_t(0));
        for (const auto& inst : r.instances) {
            // Compared with "/" separators: on Windows the extracted paths
            // are native ("\\") and Qt's are not.
            const QString extracted = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(inst->filePath)));
            QVERIFY2(extracted.startsWith(QDir::cleanPath(area.sessionDir()) + QLatin1Char('/')), qPrintable(extracted));
            QVERIFY(r.displayNames.count(inst->filePath) == 1);
        }
        QVERIFY(QString::fromStdString(r.displayNames.at(r.instances.front()->filePath))
                    .startsWith(QStringLiteral("exame do paciente.zip › EXAME/DICOM/IM")));
        // The extracted copies decode through the isolated worker like any file.
        const auto decoded = DecoderClient::decode(r.instances.front()->filePath);
        QVERIFY(decoded.ok());
        // "Fechar estudos" empties the extraction folder.
        area.clear();
        QVERIFY(QDir(area.sessionDir()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());

        // Without anyone to answer, a protected archive is reported, not opened.
        ImportTask silent;
        QSignalSpy silentDone(&silent, &ImportTask::finished);
        QVERIFY(silent.start({zip}, area.newImportDir()));
        QVERIFY(silentDone.wait(60000));
        QCOMPARE(silent.result().instances.size(), std::size_t(0));
        QCOMPARE(silent.result().issues.size(), std::size_t(1));
        QVERIFY(QString::fromStdString(silent.result().issues.front().message).contains(QStringLiteral("senha")));
    }

    void extractionAreaCleansAbandonedSessions() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        // Left behind by a session that crashed (no lock holder anymore).
        QVERIFY(QDir().mkpath(base.path() + "/sessao-antiga/importacao-1"));
        {
            QFile f(base.path() + "/sessao-antiga/importacao-1/000001");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("DICM");
        }
        QString liveDir;
        {
            ExtractionArea live(base.path());
            QVERIFY(live.valid());
            liveDir = live.sessionDir();
            QVERIFY(!QDir(base.path() + "/sessao-antiga").exists());
            // A second instance starting now must not touch a running session.
            ExtractionArea second(base.path());
            QVERIFY(second.valid());
            QVERIFY(QDir(liveDir).exists());
            QVERIFY(second.sessionDir() != liveDir);
        }
        QVERIFY(!QDir(liveDir).exists());  // removed when the program closes
    }

    void seriesPanelCollapsesAndRemembersWidth() {
        QStandardPaths::setTestModeEnabled(true);
        auto& settings = AppSettings::instance();
        settings.saveWindow({}, {});
        settings.setSeriesPanel(false, 300);
        auto* window = new MainWindow;
        window->resize(1300, 800);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* dock = window->findChild<SeriesDock*>();
        auto* grid = window->findChild<ViewerGrid*>();
        QVERIFY(dock != nullptr && grid != nullptr);
        QTRY_COMPARE(dock->width(), 300);
        dock->browser()->setDatabase(db_);
        const int gridWide = grid->width();

        // « collapses into the rail; the images gain the width.
        auto* toggle = dock->findChild<QToolButton*>(QStringLiteral("seriesDockToggle"));
        QVERIFY(toggle != nullptr);
        QTest::mouseClick(toggle, Qt::LeftButton);
        QVERIFY(dock->isCollapsed());
        QTRY_COMPARE(dock->width(), SeriesDock::kRailWidth);
        QTRY_VERIFY(grid->width() >= gridWide + 300 - SeriesDock::kRailWidth - 2);
        QAction* panelAction = nullptr;
        for (auto* a : window->findChildren<QAction*>()) {
            if (a->shortcut() == QKeySequence(Qt::Key_F2)) {
                panelAction = a;
            }
        }
        QVERIFY(panelAction != nullptr && !panelAction->isChecked());
        auto* rail = dock->findChild<QWidget*>(QStringLiteral("seriesRail"));
        QVERIFY(rail != nullptr && rail->isVisible());
        QVERIFY(!rail->grab().isNull());

        // A click anywhere on the rail brings the panel back at its width.
        QTest::mouseClick(rail, Qt::LeftButton, {}, QPoint(rail->width() / 2, rail->height() / 2));
        QVERIFY(!dock->isCollapsed());
        QVERIFY(panelAction->isChecked());
        QTRY_COMPARE(dock->width(), 300);

        // Dragged narrow: thumbnails only, no sideways scrolling, and the
        // narrow width is what F2 restores.
        window->resizeDocks({dock}, {SeriesDock::kMinExpandedWidth + 16}, Qt::Horizontal);
        QTRY_COMPARE(dock->width(), SeriesDock::kMinExpandedWidth + 16);
        QCOMPARE(dock->expandedWidth(), SeriesDock::kMinExpandedWidth + 16);
        QVERIFY(!dock->browser()->horizontalScrollBar()->isVisible());
        QVERIFY(!dock->browser()->grab().isNull());
        QTest::keyClick(window, Qt::Key_F2);
        QVERIFY(dock->isCollapsed());
        QTRY_COMPARE(dock->width(), SeriesDock::kRailWidth);
        QTest::keyClick(window, Qt::Key_F2);
        QVERIFY(!dock->isCollapsed());
        QTRY_COMPARE(dock->width(), SeriesDock::kMinExpandedWidth + 16);
        QTest::keyClick(window, Qt::Key_F2);
        QTRY_COMPARE(dock->width(), SeriesDock::kRailWidth);

        // Remembered across sessions: the next window opens collapsed and
        // expands to the same width.
        window->close();
        QCOMPARE(settings.seriesPanelCollapsed(), true);
        QCOMPARE(settings.seriesPanelWidth(), SeriesDock::kMinExpandedWidth + 16);
        delete window;
        window = new MainWindow;
        window->resize(1300, 800);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        dock = window->findChild<SeriesDock*>();
        QVERIFY(dock->isCollapsed());
        QTRY_COMPARE(dock->width(), SeriesDock::kRailWidth);
        dock->setCollapsed(false);
        QTRY_COMPARE(dock->width(), SeriesDock::kMinExpandedWidth + 16);
        delete window;
        settings.saveWindow({}, {});
        settings.setSeriesPanel(false, 0);
    }

    void planeButtonReformatsTheActiveSeries() {
        QStandardPaths::setTestModeEnabled(true);
        auto* window = new MainWindow;
        QString why;
        QVERIFY2(openWindowWith(window, tmp_.path(), &why) != nullptr, qPrintable(why));
        auto* grid = window->findChild<ViewerGrid*>();
        grid->setLayoutGrid(1, 1);
        Viewport* vp = grid->activeViewport();
        const auto* stack = dynamic_cast<StackSource*>(vp->source().get());
        QVERIFY(stack != nullptr && stack->series()->frameCount() == 16);  // series A, acquired axial
        QAction* plane = actionWithShortcut(window, QKeySequence(QStringLiteral("Ctrl+Shift+P")));
        QVERIFY(plane != nullptr && plane->isEnabled());
        auto orientationOf = [vp]() -> int {
            const auto* ms = dynamic_cast<MprSource*>(vp->source().get());
            return ms != nullptr ? static_cast<int>(ms->orientation()) : -1;
        };
        // Axial (acquired) -> sagittal -> coronal -> axial again (original images).
        plane->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(orientationOf(), static_cast<int>(MprOrientation::Sagittal), 30000);
        QTRY_VERIFY(vp->currentFrame() != nullptr);
        QVERIFY(vp->mprGuides().empty());  // a single plane has no crosshair
        QCOMPARE(plane->iconText(), QStringLiteral("Plano: Sagital"));
        saveShot(window, QStringLiteral("plane-sagittal"));
        plane->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(orientationOf(), static_cast<int>(MprOrientation::Coronal), 30000);
        plane->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(dynamic_cast<StackSource*>(vp->source().get()) != nullptr, 30000);
        QCOMPARE(plane->iconText(), QStringLiteral("Plano: Axial"));
        // Layout 1 × 3 is offered.
        QAction* oneByThree = nullptr;
        for (auto* a : window->findChildren<QAction*>()) {
            if (a->text() == QStringLiteral("1 × 3")) {
                oneByThree = a;
            }
        }
        QVERIFY(oneByThree != nullptr);
        oneByThree->trigger();
        QCOMPARE(grid->rows(), 1);
        QCOMPARE(grid->cols(), 3);
        // Toolbar: "Rolar", no capture or reset buttons any more.
        auto* tb = window->findChild<QToolBar*>(QStringLiteral("mainToolbar"));
        QVERIFY(tb != nullptr);
        QStringList labels;
        for (auto* a : tb->actions()) {
            labels << a->iconText();
            QVERIFY(a->shortcut() != QKeySequence(QStringLiteral("Ctrl+0")));
            QVERIFY(a->shortcut() != QKeySequence(QStringLiteral("Ctrl+Shift+C")));
        }
        QVERIFY2(labels.contains(QStringLiteral("Rolar")), qPrintable(labels.join(',')));
        QVERIFY(!labels.contains(QStringLiteral("Cortes")));
        delete window;
    }

    void mprMenuThicknessAndCrosshairButton() {
        QStandardPaths::setTestModeEnabled(true);
        AppSettings::instance().setMprCrosshairVisible(true);
        auto* window = new MainWindow;
        QString why;
        QVERIFY2(openWindowWith(window, tmp_.path(), &why) != nullptr, qPrintable(why));
        auto* grid = window->findChild<ViewerGrid*>();
        QMenu* mprMenu = nullptr;
        for (auto* m : window->menuBar()->findChildren<QMenu*>()) {
            if (m->title() == QStringLiteral("M&PR")) {
                mprMenu = m;
            }
        }
        QVERIFY(mprMenu != nullptr);
        // Every option is in the menu itself: no submenu that might not open.
        QAction* fiveMm = nullptr;
        for (auto* a : mprMenu->actions()) {
            QVERIFY2(a->menu() == nullptr, qPrintable(a->text()));
            if (a->text() == QStringLiteral("5 mm")) {
                fiveMm = a;
            }
        }
        QVERIFY(fiveMm != nullptr && fiveMm->isEnabled());
        // Chosen before the MPR is open: the MPR opens with that thickness.
        fiveMm->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(grid->mprSession() != nullptr, 30000);
        for (auto o : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
            QCOMPARE(grid->mprSession()->slab(o).thickness, 5.0);
        }
        QVERIFY(fiveMm->isChecked());
        if (!qEnvironmentVariable("VISUALTC_TEST_SCREENSHOTS").isEmpty()) {
            for (auto* m : window->findChildren<QMenu*>()) {
                if (m->title() == QStringLiteral("MPR")) {  // the toolbar button's copy
                    m->popup(QPoint(400, 120));
                    QTest::qWait(100);
                    saveShot(m, QStringLiteral("mpr-menu"));
                    m->hide();
                }
            }
            saveShot(window, QStringLiteral("mpr-slab5"));
        }
        // "Cruz" hides and shows the crosshair lines.
        QAction* cross = actionWithShortcut(window, QKeySequence(QStringLiteral("X")));
        QVERIFY(cross != nullptr && cross->isEnabled() && cross->isChecked());
        auto anyGuides = [grid] {
            for (auto* v : grid->visibleViewports()) {
                if (!v->mprGuides().empty()) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(anyGuides());
        cross->trigger();
        QVERIFY(!cross->isChecked());
        QVERIFY(!anyGuides());
        QVERIFY(!AppSettings::instance().mprCrosshairVisible());
        cross->trigger();
        QVERIFY(anyGuides());
        delete window;
    }

    void closeButtonRemovesOnlyThatStudy() {
        QTemporaryDir other;
        QVERIFY(other.isValid());
        QVERIFY(writeScoutExam(other.path()));
        QStandardPaths::setTestModeEnabled(true);
        auto* window = new MainWindow;
        window->resize(1200, 800);
        window->show();
        window->importPaths({tmp_.path(), other.path()});
        auto* browser = window->findChild<SeriesBrowser*>();
        QTRY_COMPARE_WITH_TIMEOUT(browser->seriesCount(), 4, 30000);
        auto* grid = window->findChild<ViewerGrid*>();
        QTRY_VERIFY_WITH_TIMEOUT(grid->activeViewport()->currentFrame() != nullptr, 30000);
        const QPoint x = browser->closeButtonCenter(0);
        QVERIFY(x.x() > 0);
        QTest::mouseClick(browser->viewport(), Qt::LeftButton, Qt::NoModifier, x);
        QTRY_COMPARE(browser->seriesCount(), 2);
        // Nothing of the closed study is left on screen.
        QSet<QString> remaining;
        for (int i = 0; i < browser->topLevelItemCount(); ++i) {
            const QString id = browser->topLevelItem(i)->data(0, Qt::UserRole).toString();
            if (!id.isEmpty()) {
                remaining.insert(id);
            }
        }
        QCOMPARE(remaining.size(), 2);
        for (auto* vp : window->findChildren<Viewport*>()) {
            if (const auto* st = dynamic_cast<StackSource*>(vp->source().get())) {
                QVERIFY(remaining.contains(QString::fromStdString(st->series()->id)));
            }
        }
        // It can be opened again.
        window->importPaths({tmp_.path(), other.path()});
        QTRY_COMPARE_WITH_TIMEOUT(browser->seriesCount(), 4, 30000);
        delete window;
    }

    void accentColorIsAppliedToTheInterface() {
        const QString oldSheet = qApp->styleSheet();
        const QPalette oldPalette = qApp->palette();
        const QString oldStyle = qApp->style()->name();
        for (Accent a : kAccents) {
            Theme::apply(*qApp, true, a);
            QCOMPARE(Theme::colors().accent, Theme::accentColor(a, true));
            QVERIFY(qApp->styleSheet().contains(Theme::accentColor(a, true).name(), Qt::CaseInsensitive));
            QCOMPARE(Theme::accentFromKey(Theme::accentKey(a)), a);
            QVERIFY(!Theme::accentName(a).isEmpty());
            // Readable: the dim variant (checked buttons) stays dark.
            QVERIFY(Theme::colors().accentDim.lightness() < 120);
        }
        QCOMPARE(Theme::accentFromKey(QStringLiteral("desconhecida")), Accent::Blue);
        // Preferences offer the colours and the three languages.
        {
            PreferencesDialog dlg;
            dlg.show();
            bool colours = false;
            bool languages = false;
            for (auto* combo : dlg.findChildren<QComboBox*>()) {
                colours = colours || combo->findText(QStringLiteral("Verde neon")) >= 0;
                languages = languages || (combo->findText(QStringLiteral("Español")) >= 0 &&
                                          combo->findText(QStringLiteral("English")) >= 0 &&
                                          combo->findText(QStringLiteral("Português (Brasil)")) >= 0);
            }
            QVERIFY(colours);
            QVERIFY(languages);
            saveShot(&dlg, QStringLiteral("preferences"));
        }
        qApp->setStyleSheet(oldSheet);
        qApp->setPalette(oldPalette);
        QApplication::setStyle(oldStyle);
    }

    void systemFileOpenEventImportsExam() {
        // macOS hands documents over as QFileOpenEvent ("Abrir com", a CD
        // folder dropped on the Dock icon, double-click on a .dcm).
        QStandardPaths::setTestModeEnabled(true);
        auto* window = new MainWindow;
        window->show();
        auto* browser = window->findChild<SeriesBrowser*>();
        QVERIFY(browser != nullptr);
        QCOMPARE(browser->seriesCount(), 0);
        QFileOpenEvent open(tmp_.path());
        QCoreApplication::sendEvent(qApp, &open);
        QTRY_COMPARE_WITH_TIMEOUT(browser->seriesCount(), 2, 30000);
        delete window;
    }

    // Last: installs the Spanish translation for the rest of the run.
    void spanishAndEnglishTranslations() {
        QCOMPARE(languageFromKey(QStringLiteral("es")), Language::Spanish);
        QCOMPARE(languageKey(Language::English), QStringLiteral("en"));
        installLanguage(*qApp, Language::Spanish);
        QVERIFY(translationSize() > 400);
        QCOMPARE(currentLanguage(), Language::Spanish);
        QCOMPARE(QCoreApplication::translate("MainWindow", "Rolar"), QStringLiteral("Desplazar"));
        QCOMPARE(QCoreApplication::translate("SeriesBrowser", "%n imagem(ns)", nullptr, 1), QStringLiteral("1 imagen"));
        QCOMPARE(QCoreApplication::translate("SeriesBrowser", "%n imagem(ns)", nullptr, 3), QStringLiteral("3 imágenes"));
        // Messages of the core, with their variable parts.
        QCOMPARE(trCore(std::string("Arquivo DICOM corrompido (sequência corrompida).")),
                 QStringLiteral("Archivo DICOM dañado (secuencia dañada)."));
        QCOMPARE(trCore(std::string("O arquivo compactado exame 1.zip está protegido por senha.")),
                 QStringLiteral("El archivo comprimido exame 1.zip está protegido con contraseña."));
        QCOMPARE(trCore(QStringLiteral("texto que não é do VisualTC")), QStringLiteral("texto que não é do VisualTC"));
        QCOMPARE(QLocale().decimalPoint(), QStringLiteral(","));
        // The window itself.
        QStandardPaths::setTestModeEnabled(true);
        auto* window = new MainWindow;
        QCOMPARE(window->menuBar()->actions().front()->text(), QStringLiteral("&Archivo"));
        auto* browser = window->findChild<SeriesBrowser*>();
        browser->setDatabase(db_);
        bool found = false;
        for (int i = 0; i < browser->topLevelItemCount(); ++i) {
            found = found || browser->topLevelItem(i)->toolTip(0).contains(QStringLiteral("16 imágenes"));
        }
        QVERIFY(found);
        delete window;
    }

    void cleanupTestCase() {
        frames_->shutdown();
    }
};

QTEST_MAIN(UiTests)
#include "test_ui.moc"
