#ifndef OPENKJ_STARTUPLOGGING_H
#define OPENKJ_STARTUPLOGGING_H

#include <QString>
#include <spdlog/common.h>

namespace okj {

struct LogDirectories {
    QString configured;
    QString standard;
    QString fallback;
};

struct FileLog {
    spdlog::sink_ptr sink;
    QString path;
    QString diagnostic;
};

QString fallbackLogDirectory();
FileLog openFileLog(const LogDirectories &directories, const QString &filename,
                    spdlog::level::level_enum level, const char *pattern);
void reportLogFallback(const FileLog &log);

} // namespace okj

#endif
