#include <algorithm>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QDebug>
#include <QDate>
#include "mainwindow.h"
#include <QStyleFactory>
#include <QStringList>
#include <QMessageBox>
#include <QSqlDatabase>
#include "settings.h"
#include "idledetect.h"
#include "runguard/runguard.h"
#include "okjversion.h"
#include "bundledruntime.h"
#include "startupsplash.h"
#include "mediainitialization.h"
#include "databaseschema.h"
#include "startuplogging.h"
#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async_logger.h>
#include <spdlog/async.h>


Settings settings;
IdleDetect *filter;

//todo: This should be me moved into main after migration to spdlog is complete
//      It's currently only global for use by the QDebug callback
std::shared_ptr<spdlog::async_logger> logger;


void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    bool loggingEnabled = settings.logEnabled();
    std::string logMsg = msg.toStdString();
    if (context.function) {
        logMsg.append(" [");
        logMsg.append(context.function);
        logMsg.append("]");
    }
    switch (type) {
        case QtDebugMsg:
            if (!loggingEnabled)
                return;
            logger->debug(logMsg);
            break;
        case QtInfoMsg:
            logger->info(logMsg);
            break;
        case QtWarningMsg:
            logger->warn(logMsg);
            break;
        case QtCriticalMsg:
            logger->critical(logMsg);
            break;
        case QtFatalMsg:
            logger->critical(logMsg);
          //  abort();
    }
}


#ifdef Q_OS_MACOS

static void configureBundledGStreamer()
{
    // OpenKJ.app/Contents/MacOS/openkj
    const QDir macOSDir(
        QCoreApplication::applicationDirPath()
    );

    const QDir contentsDir(
        macOSDir.filePath("..")
    );

    const QString pluginDir = contentsDir.filePath(
        "PlugIns"
    );

    const QString scannerPath = contentsDir.filePath(
        "Helpers/gst-plugin-scanner"
    );

    // Detect our packaged runtime.
    // Development builds use Homebrew plus our custom SoundTouch plugin.
    const QString corePlugin = QDir(pluginDir).filePath(
        "libgstcoreelements.dylib"
    );

    if (!QFileInfo::exists(corePlugin) ||
        !QFileInfo::exists(scannerPath)) {
        const QString soundTouchPlugin = QDir(pluginDir).filePath(
            "libgstsoundtouch.dylib"
        );
        if (QFileInfo::exists(soundTouchPlugin)) {
            // Add the app's custom plugin before GStreamer initializes, while
            // retaining the system plugins and any user-provided plugin paths.
            QByteArray paths = QFile::encodeName(pluginDir);
            const QByteArray existingPaths = qEnvironmentVariableIsSet("GST_PLUGIN_PATH_1_0")
                ? qgetenv("GST_PLUGIN_PATH_1_0") : qgetenv("GST_PLUGIN_PATH");
            if (!existingPaths.isEmpty())
                paths += ':' + existingPaths;
            qputenv("GST_PLUGIN_PATH_1_0", paths);
            qInfo() << "[GStreamer] Custom SoundTouch plugin:" << soundTouchPlugin;
        }
        qInfo() << "[GStreamer] Using system runtime";
        return;
    }

    const QString cacheDir =
        QStandardPaths::writableLocation(
            QStandardPaths::CacheLocation
        );

    if (cacheDir.isEmpty() || !QDir().mkpath(cacheDir)) {
        qWarning() << "[GStreamer] Cannot create registry directory";
        return;
    }

    const QString registryPath =
        QDir(cacheDir).filePath("gst-registry.bin");

    // Prevent inherited paths from loading external plugins.
    qputenv("GST_PLUGIN_PATH_1_0", QByteArray());

    qputenv(
        "GST_PLUGIN_SYSTEM_PATH_1_0",
        QFile::encodeName(pluginDir)
    );

    qputenv(
        "GST_PLUGIN_SCANNER",
        QFile::encodeName(scannerPath)
    );

    qputenv(
        "GST_REGISTRY_1_0",
        QFile::encodeName(registryPath)
    );

    qInfo() << "[GStreamer] Using bundled runtime";
    qInfo() << "[GStreamer] Plugins:" << pluginDir;
    qInfo() << "[GStreamer] Scanner:" << scannerPath;
}

#endif


static spdlog::level::level_enum logLevel(int setting)
{
    switch (setting) {
    case Settings::LOG_LEVEL_CRITICAL: return spdlog::level::critical;
    case Settings::LOG_LEVEL_ERROR: return spdlog::level::err;
    case Settings::LOG_LEVEL_WARNING: return spdlog::level::warn;
    case Settings::LOG_LEVEL_INFO: return spdlog::level::info;
    case Settings::LOG_LEVEL_DEBUG: return spdlog::level::debug;
    case Settings::LOG_LEVEL_TRACE: return spdlog::level::trace;
    default: return spdlog::level::off;
    }
}

int main(int argc, char *argv[]) {
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern("[%^%l%$] %v");
    const auto consoleLevel = logLevel(settings.getConsoleLogLevel());
    const auto fileLevel = logLevel(settings.getFileLogLevel());
    okj::FileLog fileLog;
    if (fileLevel != spdlog::level::off) {
        const auto name = QStringLiteral("openkj-debug-") + QDate::currentDate().toString(Qt::ISODate) + ".log";
        fileLog = okj::openFileLog({settings.logDir(), Settings::defaultLogDir(), okj::fallbackLogDirectory()},
                                  name, fileLevel, "[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        okj::reportLogFallback(fileLog);
    }

    // Keep warnings visible if file logging was requested but no file could be opened.
    console->set_level(fileLevel != spdlog::level::off && !fileLog.sink
                           ? std::min(consoleLevel, spdlog::level::warn) : consoleLevel);
    std::vector<spdlog::sink_ptr> sinks{console};
    if (fileLog.sink)
        sinks.push_back(fileLog.sink);
    spdlog::init_thread_pool(8192, 2);
    logger = std::make_shared<spdlog::async_logger>("logger", sinks.begin(), sinks.end(), spdlog::thread_pool(),
                                                    spdlog::async_overflow_policy::block);
    spdlog::register_logger(logger);
    logger->set_level(spdlog::level::trace);
    spdlog::flush_every(std::chrono::seconds(1));
    logger->flush_on(spdlog::level::warn);

    logger->info("OpenKJ version {} starting up", OKJ_VERSION_STRING);


    //QLoggingCategory::setFilterRules("*.debug=true");
    qInstallMessageHandler(myMessageOutput);
    QApplication a(argc, argv);
    configureWindowsBundledGStreamer();
    #ifdef Q_OS_MACOS
    configureBundledGStreamer();
    #endif

    filter = new IdleDetect(&a);
    a.installEventFilter(filter);
    if (settings.theme() == 1) {
        QPalette palette;
        QApplication::setStyle(QStyleFactory::create("Fusion"));
        palette.setColor(QPalette::Window, QColor(53, 53, 53));
        palette.setColor(QPalette::WindowText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
        palette.setColor(QPalette::Base, QColor(42, 42, 42));
        palette.setColor(QPalette::AlternateBase, QColor(66, 66, 66));
        palette.setColor(QPalette::ToolTipBase, Qt::white);
        palette.setColor(QPalette::ToolTipText, QColor(53, 53, 53));
        palette.setColor(QPalette::Text, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
        palette.setColor(QPalette::Dark, QColor(35, 35, 35));
        palette.setColor(QPalette::Shadow, QColor(20, 20, 20));
        palette.setColor(QPalette::Button, QColor(53, 53, 53));
        palette.setColor(QPalette::ButtonText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, QColor(42, 130, 218));
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(127, 127, 127));
        QApplication::setPalette(palette);
    } else if (settings.theme() == 2) {
        QApplication::setStyle(QStyleFactory::create("Fusion"));
    }
//    else
//    {
//
//    }
    QApplication::setFont(settings.applicationFont(), "QWidget");
    QApplication::setFont(settings.applicationFont(), "QMenu");
    QApplication::setFont(settings.applicationFont(), "QAction");

    // RunGuard seems to be broken by flatpak, ignore it if running in a flatpak sandbox
    logger->info("App dir path {}", QCoreApplication::applicationDirPath().toStdString());
    // Keep protection through the event loop and main-window teardown.
    std::unique_ptr<RunGuard> guard;
    if (QCoreApplication::applicationDirPath() == "/app/bin") {
        logger->info("RunGuard disabled due to flatpak sandbox");
    } else {
        logger->debug("Checking for other instances");
        guard = std::make_unique<RunGuard>("SharedMemorySingleInstanceProtectorOpenKJ");
        if (!guard->tryToRun()) {
            QMessageBox msgBox;
            msgBox.setText("OpenKJ is already running!");
            msgBox.setInformativeText(
                    "In order to protect the database, you can only run one instance of OpenKJ at a time.\nExiting now.");
            msgBox.setIcon(QMessageBox::Critical);
            msgBox.exec();
            return 1;
        }
    }
    if (!settings.lastStartupOk()) {
        QMessageBox msgBox;
        msgBox.setText("OpenKJ appears to have failed to startup on the last run.");
        msgBox.setInformativeText("Would you like to attempt to recover by loading safe settings?");
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::Yes);
        Settings::setSafeStartupMode(msgBox.exec() == QMessageBox::Yes);
    }
#ifdef Q_OS_DARWIN
    Settings::setSkipLegacyUiRestoreOnUpgrade(settings.lastRunVersion() != OKJ_VERSION_STRING);
#endif
    settings.setLastRunVersion(OKJ_VERSION_STRING);
    settings.setStartupOk(false);

    // Keep the GUI event loop running while GStreamer builds its plugin cache.
    // MainWindow and its playback pipelines are constructed on the GUI thread
    // only after media initialization has finished.
    StartupSplash splash;
    splash.show();
    // QSplashScreen::repaint delivers the initial window events before any
    // main-window objects exist; no event pumping is needed during construction.
    splash.repaint();

    std::unique_ptr<MainWindow> mainWindow;
    QFutureWatcher<MediaInitializationResult> mediaInitialization;
    QObject::connect(&mediaInitialization, &QFutureWatcher<MediaInitializationResult>::finished, &a, [&] {
        const auto result = mediaInitialization.result();
        if (!result.success) {
            logger->error("GStreamer initialization failed: {}", result.error.toStdString());
            splash.hide();
            QMessageBox::critical(nullptr, QObject::tr("Unable to start OpenKJ"), result.error);
            a.exit(1);
            return;
        }
        logger->info("GStreamer initialization completed in {} ms", result.elapsedMs);
        QElapsedTimer startupTimer;
        startupTimer.start();
        QCoreApplication::setOrganizationName("OpenKJ");
        QCoreApplication::setOrganizationDomain("OpenKJ.org");
        QCoreApplication::setApplicationName("OpenKJ");
        const auto dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const auto path = dataDir.isEmpty() ? QString() : QDir(dataDir).filePath("openkj.sqlite");
        auto database = QSqlDatabase::addDatabase("QSQLITE");
        const auto error = path.isEmpty() ? QObject::tr("No application data directory is available.")
                                          : okj::openAndMigrateDatabase(database, path);
        if (!error.isEmpty()) {
            logger->error("Database startup failed for {}: {}", path.toStdString(), error.toStdString());
            splash.hide();
            QMessageBox message(QMessageBox::Critical, QObject::tr("Unable to open the OpenKJ database"),
                                QObject::tr("OpenKJ could not open or upgrade its database."));
            message.setTextFormat(Qt::PlainText);
            message.setInformativeText(path.isEmpty() ? error : QStringLiteral("%1\n\n%2").arg(path, error));
            message.exec();
            a.exit(1);
            return;
        }
        logger->info("Database initialization completed in {} ms", startupTimer.restart());
        mainWindow = std::make_unique<MainWindow>(database);
        logger->info("Main window initialization completed in {} ms", startupTimer.elapsed());
        splash.finishWhenReady(mainWindow.get());
    });
    mediaInitialization.setFuture(initializeMediaAsync());

    const int exitCode = QApplication::exec();
    // Finish the worker before destroying QApplication, including an early quit.
    mediaInitialization.waitForFinished();
    if (!mainWindow && exitCode == 0)
        settings.setStartupOk(true);
    return exitCode;
}
