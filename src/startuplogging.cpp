#include "startuplogging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <cstdio>
#include <spdlog/sinks/basic_file_sink.h>

namespace okj {
namespace {
QString absoluteDirectory(const QString &directory)
{
    return directory.trimmed().isEmpty() ? QString{} : QDir::cleanPath(QDir(directory).absolutePath());
}

FileLog tryOpenLog(const QString &directory, const QString &filename, bool createDirectory,
                   spdlog::level::level_enum level, const char *pattern)
{
    if (directory.isEmpty())
        return {{}, {}, QStringLiteral("the log directory is unset")};

    if (!QFileInfo(directory).isDir() && (!createDirectory || !QDir().mkpath(directory)))
        return {{}, {}, QStringLiteral("log directory %1 is unavailable").arg(QDir::toNativeSeparators(directory))};

    const auto path = QDir(directory).filePath(filename);
    try {
#if defined(_WIN32) && defined(SPDLOG_WCHAR_FILENAMES)
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.toStdWString());
#else
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(QFile::encodeName(path).toStdString());
#endif
        sink->set_pattern(pattern);
        sink->set_level(level);
        return {sink, path, {}};
    } catch (const spdlog::spdlog_ex &error) {
        return {{}, {}, QStringLiteral("could not open %1 (%2)")
                            .arg(QDir::toNativeSeparators(path), QString::fromLocal8Bit(error.what()))};
    }
}
}

QString fallbackLogDirectory()
{
    const auto location = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return location.isEmpty() ? QString{} : QDir(location).filePath(QStringLiteral("logs"));
}

FileLog openFileLog(const LogDirectories &directories, const QString &filename,
                    spdlog::level::level_enum level, const char *pattern)
{
    if (level == spdlog::level::off)
        return {};

    const auto configured = absoluteDirectory(directories.configured);
    const auto fallback = absoluteDirectory(directories.fallback);
    // The directory picker selects an existing custom folder. Avoid recreating
    // a missing mount point, while allowing OpenKJ's standard paths to be made.
    const bool standardPath = configured == absoluteDirectory(directories.standard) || configured == fallback;
    auto file = tryOpenLog(configured, filename, standardPath, level, pattern);
    if (file.sink)
        return file;

    if (!fallback.isEmpty() && fallback != configured) {
        auto alternative = tryOpenLog(fallback, filename, true, level, pattern);
        if (alternative.sink) {
            alternative.diagnostic = QStringLiteral("OpenKJ logging: %1. Writing to %2.")
                .arg(file.diagnostic, QDir::toNativeSeparators(alternative.path));
            return alternative;
        }
        file.diagnostic += QStringLiteral("; fallback failed: %1").arg(alternative.diagnostic);
    }

    file.diagnostic = QStringLiteral("OpenKJ logging: %1. Continuing with console diagnostics.")
        .arg(file.diagnostic);
    return file;
}

void reportLogFallback(const FileLog &log)
{
    if (log.diagnostic.isEmpty())
        return;
    const auto message = log.diagnostic.toUtf8();
    std::fprintf(stderr, "%s\n", message.constData());
    if (log.sink) {
        // Report the fallback once even when the configured file filter is
        // stricter than warning, then leave that filter in place.
        try {
            log.sink->log(spdlog::details::log_msg("logger", spdlog::level::warn,
                          spdlog::string_view_t(message.constData(), message.size())));
            log.sink->flush();
        } catch (const spdlog::spdlog_ex &error) {
            std::fprintf(stderr, "OpenKJ logging: could not write the startup diagnostic (%s).\n", error.what());
        }
    }
}

} // namespace okj
