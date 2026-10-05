// VisualTC — DICOM Medical Image Viewer
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <QStandardPaths>
#include <QTimer>

#include "app/AppSettings.h"
#include "app/Theme.h"
#include "core/Logger.h"
#include "core/PathUtil.h"
#include "dicom/DicomParser.h"
#include "io/DecoderClient.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("VisualTC"));
    QApplication::setOrganizationDomain(QStringLiteral("visualtc.org"));
    QApplication::setApplicationName(QStringLiteral("VisualTC"));
    QApplication::setApplicationDisplayName(QStringLiteral("VisualTC"));
    QApplication::setApplicationVersion(QStringLiteral(VISUALTC_VERSION));
    QLocale::setDefault(QLocale(QLocale::Portuguese, QLocale::Brazil));
    // Standard Qt dialogs/buttons in Portuguese when the Qt translations are
    // deployed (qtbase_pt_BR.qm); silently falls back to English otherwise.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)) ||
        qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QCoreApplication::applicationDirPath() + QStringLiteral("/translations"))) {
        QApplication::installTranslator(&qtTranslator);
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("VisualTC — DICOM Medical Image Viewer"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("caminhos"), QStringLiteral("Arquivos ou pastas DICOM a abrir."),
                                 QStringLiteral("[caminhos...]"));
    const QCommandLineOption debugOpt(QStringLiteral("debug"), QStringLiteral("Log detalhado (nível DEBUG)."));
    const QCommandLineOption noIsolation(QStringLiteral("no-isolation"),
                                         QStringLiteral("Decodificar no próprio processo (diagnóstico)."));
    const QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                        QStringLiteral("Salva uma captura da janela e encerra (testes)."),
                                        QStringLiteral("arquivo.png"));
    const QCommandLineOption layout(QStringLiteral("layout"), QStringLiteral("Layout inicial, ex.: 2x2."),
                                    QStringLiteral("RxC"));
    const QCommandLineOption mpr(QStringLiteral("mpr"), QStringLiteral("Abre o MPR da maior série volumétrica."));
    const QCommandLineOption sync(QStringLiteral("sync"), QStringLiteral("Ativa a sincronização."));
    const QCommandLineOption demo(QStringLiteral("demo-measurements"),
                                  QStringLiteral("Desenha medidas de demonstração (testes)."));
    const QCommandLineOption slab(QStringLiteral("slab"), QStringLiteral("Espessura MIP do MPR em mm (testes)."),
                                  QStringLiteral("mm"));
    const QCommandLineOption preset(QStringLiteral("preset"), QStringLiteral("Preset de janela de TC (testes)."),
                                    QStringLiteral("nome"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Tamanho da janela, ex.: 1600x1000."),
                                  QStringLiteral("LxA"));
    parser.addOptions({debugOpt, noIsolation, screenshot, layout, mpr, sync, demo, slab, preset, size});
    parser.process(app);

    // Local log, without patient data (see docs/ARCHITECTURE.md).
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
    QDir().mkpath(logDir);
    auto& log = vtc::Logger::instance();
    log.openFile(vtc::utf8ToPath((logDir + "/visualtc.log").toStdString()));
    log.setLevel(parser.isSet(debugOpt) ? vtc::LogLevel::Debug : vtc::LogLevel::Info);
    log.setEchoToStderr(parser.isSet(debugOpt));
    vtc::logInfo("app", "VisualTC " VISUALTC_VERSION " iniciado.");

    vtc::initializeDicomLibrary();
    auto& settings = vtc::AppSettings::instance();
    vtc::Theme::apply(app, settings.darkTheme());
    if (settings.fontPointSize() > 0) {
        QFont f = QApplication::font();
        f.setPointSize(settings.fontPointSize());
        QApplication::setFont(f);
    }
    QApplication::setWindowIcon(vtc::Icons::appIcon());
    vtc::DecoderClient::initialize(settings.useIsolatedDecoder() && !parser.isSet(noIsolation));

    vtc::MainWindow window;
    vtc::AutomationOptions automation;
    automation.screenshot = parser.value(screenshot);
    if (parser.isSet(layout)) {
        const QStringList rc = parser.value(layout).toLower().split('x');
        if (rc.size() == 2) {
            automation.rows = rc[0].toInt();
            automation.cols = rc[1].toInt();
        }
    }
    automation.mpr = parser.isSet(mpr);
    automation.sync = parser.isSet(sync);
    automation.demoMeasurements = parser.isSet(demo) ? QStringLiteral("1") : QString();
    automation.slab = parser.value(slab).toInt();
    automation.preset = parser.value(preset);
    window.setAutomation(automation);
    if (parser.isSet(size)) {
        const QStringList wh = parser.value(size).toLower().split('x');
        if (wh.size() == 2) {
            window.resize(wh[0].toInt(), wh[1].toInt());
        }
    }
    window.show();

    const QStringList paths = parser.positionalArguments();
    if (!paths.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, paths] { window.importPaths(paths); });
    }
    const int rc = QApplication::exec();
    vtc::logInfo("app", "VisualTC encerrado.");
    return rc;
}
