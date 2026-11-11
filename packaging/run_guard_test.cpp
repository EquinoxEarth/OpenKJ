#include "runguard/runguard.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int child(QCoreApplication &app, const QStringList &arguments)
{
    RunGuard guard(arguments.at(2));
    if (!guard.tryToRun())
        return 1;
    if (arguments.at(1) == "--probe")
        return 0;

    // Signal readiness from the event loop, while retaining the guard until exit.
    QTimer::singleShot(0, [] {
        std::puts("ready");
        std::fflush(stdout);
    });
    QTimer shutdown;
    QObject::connect(&shutdown, &QTimer::timeout, &app, [&] {
        if (QFile::exists(arguments.at(3)))
            app.quit();
    });
    shutdown.start(10);
    QTimer::singleShot(15000, &app, [] { QCoreApplication::exit(2); });
    return app.exec();
}

void probe(const QString &key, int expectedExitCode)
{
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(), {"--probe", key});
    require(process.waitForStarted(3000), "Cannot start competing process");
    require(process.waitForFinished(3000), "Competing process did not exit");
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == expectedExitCode,
            "Competing process returned an unexpected guard result");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 3 && arguments.at(1) == "--probe")
        return child(app, arguments);
    if (arguments.size() == 4 && arguments.at(1) == "--hold")
        return child(app, arguments);

    try {
        // Unique IPC keys keep this check independent of any running OpenKJ.
        const QString key = "OpenKJ-RunGuard-Test-" + QUuid::createUuid().toString();
        QTemporaryDir directory;
        require(directory.isValid(), "Cannot create isolated shutdown fixture");
        const auto quitPath = directory.filePath("quit");
        QProcess first;
        first.start(QCoreApplication::applicationFilePath(), {"--hold", key, quitPath});
        require(first.waitForStarted(3000), "Cannot start first instance");
        QByteArray output;
        while (!output.contains("ready\n")) {
            require(first.waitForReadyRead(3000), "First instance did not enter its event loop");
            output += first.readAllStandardOutput();
        }
        probe(key, 1);
        // A rejected process must not release the first instance's protection.
        probe(key, 1);
        require(first.state() == QProcess::Running, "First instance exited unexpectedly");

        QFile quitFile(quitPath);
        require(quitFile.open(QIODevice::WriteOnly), "Cannot request clean shutdown");
        quitFile.close();
        require(first.waitForFinished(3000), "First instance did not shut down cleanly");
        require(first.exitStatus() == QProcess::NormalExit && first.exitCode() == 0,
                "First instance failed during shutdown");
        probe(key, 0);
        probe(key, 0);
        std::puts("RunGuard rejects concurrent instances and permits relaunch after clean exit");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
