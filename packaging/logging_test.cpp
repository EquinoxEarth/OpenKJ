#include "logging.h"
#include "startuplogging.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <spdlog/async.h>
#include <spdlog/sinks/ostream_sink.h>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void checkStartupLogFallback()
{
    QTemporaryDir test;
    require(test.isValid(), "Cannot create an isolated startup logging directory");
    const auto standard = test.filePath("Documents/OpenKJ_Logs");
    const auto fallback = test.filePath("app-data/logs");
    const auto disconnected = test.filePath("removed-volume/custom-logs");
    const auto filename = QStringLiteral("openkj-debug-test.log");

    const auto disabled = okj::openFileLog({disconnected, standard, fallback}, filename,
                                            spdlog::level::off, "%v");
    require(!disabled.sink && disabled.diagnostic.isEmpty(), "Disabled file logging must skip file setup");
    require(!QFile::exists(test.filePath("removed-volume")) && !QFile::exists(test.filePath("app-data")),
            "Disabled logging must leave both paths alone");

    const auto normal = okj::openFileLog({standard, standard, fallback}, filename,
                                          spdlog::level::info, "%v");
    require(normal.sink && normal.path == QDir(standard).filePath(filename) && normal.diagnostic.isEmpty(),
            "The normal log directory should be created and used");

    const auto recovered = okj::openFileLog({disconnected, standard, fallback}, filename,
                                             spdlog::level::critical, "%v");
    require(recovered.sink && recovered.path == QDir(fallback).filePath(filename) &&
                recovered.diagnostic.contains(QDir::toNativeSeparators(disconnected)),
            "An unavailable custom directory should fall back with a useful diagnostic");
    require(!QFile::exists(test.filePath("removed-volume")), "A missing custom volume should stay absent");
    okj::reportLogFallback(recovered);
    require(recovered.sink->level() == spdlog::level::critical, "Fallback must preserve the file log threshold");
    QFile saved(recovered.path);
    require(saved.open(QIODevice::ReadOnly) && saved.readAll().contains("removed-volume"),
            "The fallback file should retain the startup diagnostic");

    const auto unwritable = test.filePath("unwritable");
    require(QDir().mkpath(QDir(unwritable).filePath(filename)), "Cannot block the configured log file");
    const auto fileError = okj::openFileLog({unwritable, standard, fallback}, filename,
                                             spdlog::level::info, "%v");
    require(fileError.sink && fileError.diagnostic.contains("could not open"),
            "A file sink that cannot open its file should use the fallback");

    const auto blockedFallback = test.filePath("blocked-fallback");
    QFile blocker(blockedFallback);
    require(blocker.open(QIODevice::WriteOnly), "Cannot block the fallback directory");
    blocker.close();
    const auto consoleOnly = okj::openFileLog({unwritable, standard, blockedFallback}, filename,
                                               spdlog::level::info, "%v");
    require(!consoleOnly.sink && consoleOnly.diagnostic.contains("fallback failed") &&
                consoleOnly.diagnostic.contains("console diagnostics"),
            "Failure of both locations must leave a console diagnostic");
}

}

int main()
{
    try {
        checkStartupLogFallback();
        std::ostringstream output;
        std::string error;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
        auto pool = std::make_shared<spdlog::details::thread_pool>(128, 1);
        auto logger = std::make_shared<spdlog::async_logger>(
            "logging-test", sink, pool, spdlog::async_overflow_policy::block);
        logger->set_pattern("%v");
        logger->set_error_handler([&](const std::string &message) { error = message; });

        auto singer = QStringLiteral("Zoë 李 {karaoke}");
        const auto originalSinger = singer.toUtf8().toStdString();
        const auto path = QStringLiteral("/music/Björk/夜 {}.zip");
        logger->info("{} | {}", singer, path);
        singer = QStringLiteral("changed after enqueue");
        logger->info("temporary {}", QStringLiteral("Élodie"));
        logger->info("aligned [{:>8}] num={:03} flag={:5}", QStringLiteral("Jo"), 7, true);
        logger->info(std::string("Qt message has {braces} and 100%"));
        const auto binaryText = QStringLiteral("A") + QChar(0) + QStringLiteral("B");
        logger->info("embedded [{}]", binaryText);
        logger->flush();
        pool.reset(); // Joins the worker after it drains messages and the flush request.

        const auto text = output.str();
        require(error.empty(), "Logging must not report formatting errors");
        require(text.find(originalSinger + " | " + path.toUtf8().toStdString()) != std::string::npos,
                "Async logs must retain Unicode singers, paths, and literal braces");
        require(text.find(QStringLiteral("temporary Élodie").toUtf8().toStdString()) != std::string::npos,
                "Temporary Qt strings must survive async logging");
        require(text.find("aligned [      Jo] num=007 flag=true") != std::string::npos,
                "Qt strings must retain alignment alongside numeric and boolean formatting");
        require(text.find("Qt message has {braces} and 100%") != std::string::npos,
                "Qt message forwarding must treat runtime message text literally");
        require(text.find("embedded [" + binaryText.toUtf8().toStdString() + "]") != std::string::npos,
                "Qt string formatting must not truncate embedded nulls");
        std::puts("Async Qt logging checks passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
