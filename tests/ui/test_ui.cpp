// Integration tests of the Qt layer, run headless (QT_QPA_PLATFORM=offscreen).
#include <QAction>
#include <QDir>
#include <QFileOpenEvent>
#include <QMenu>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <cmath>

#include "app/AppSettings.h"
#include "core/PathUtil.h"
#include "dicom/DicomParser.h"
#include "dicom/DicomStudy.h"
#include "fixtures/DicomFixtures.h"
#include "io/DecoderClient.h"
#include "io/FrameProvider.h"
#include "io/ExtractionArea.h"
#include "io/ImportTask.h"
#include "ui/MainWindow.h"
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

    void isolatedDecoderSurvivesCrash() {
        const std::string path = seriesA_->frames.front().instance->filePath;
        QVERIFY(DecoderClient::decode(path).ok());
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

    void cleanupTestCase() {
        frames_->shutdown();
    }
};

QTEST_MAIN(UiTests)
#include "test_ui.moc"
