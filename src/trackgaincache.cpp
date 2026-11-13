#include "trackgaincache.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <cmath>

namespace {
quint64 cacheGeneration = 0;

QString cachePath(const QString &libraryPath)
{
    if (libraryPath.isEmpty() || libraryPath == ":memory:")
        return {};
    return QFileInfo(libraryPath).absoluteDir().filePath("audio-normalization.sqlite");
}

template<typename Operation>
void withCache(const QString &path, bool readOnly, Operation operation)
{
    if (path.isEmpty() || (readOnly && !QFileInfo::exists(path)))
        return;
    const QString name = QStringLiteral("openkj_track_gain_cache");
    const auto removeConnection = qScopeGuard([&] { QSqlDatabase::removeDatabase(name); });
    auto database = QSqlDatabase::addDatabase("QSQLITE", name);
    database.setDatabaseName(path);
    database.setConnectOptions(readOnly ? "QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=0"
                                        : "QSQLITE_BUSY_TIMEOUT=0");
    if (database.open()) {
        QSqlQuery query(database);
        operation(query);
    }
}

bool isCurrent(const okj::TrackGainKey &key)
{
    return !key.path.isEmpty() && okj::TrackGainKey::fromFile(key.path) == key;
}

bool validGain(double gain)
{
    return std::isfinite(gain) && gain >= 0.0 && gain <= 1.0;
}
}

okj::TrackGainKey okj::TrackGainKey::fromFile(const QString &path)
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile() || !info.isReadable())
        return {};
    return {info.absoluteFilePath(), info.size(), info.lastModified().toMSecsSinceEpoch()};
}

std::optional<double> okj::cachedTrackGain(const QString &libraryPath, const TrackGainKey &key)
{
    std::optional<double> result;
    if (!isCurrent(key))
        return result;
    withCache(cachePath(libraryPath), true, [&](QSqlQuery &query) {
        if (!query.prepare("SELECT gain FROM track_gain_v1 WHERE path = ? AND size = ? AND modified_ms = ?"))
            return;
        query.addBindValue(key.path);
        query.addBindValue(key.size);
        query.addBindValue(key.modifiedMs);
        if (query.exec() && query.next()) {
            bool ok = false;
            const auto gain = query.value(0).toDouble(&ok);
            if (ok && validGain(gain))
                result = gain;
        }
    });
    return result;
}

void okj::cacheTrackGain(const QString &libraryPath, const TrackGainKey &key, double gain)
{
    if (!validGain(gain) || !isCurrent(key))
        return;
    withCache(cachePath(libraryPath), false, [&](QSqlQuery &query) {
        // Change the table version if the gain calculation changes.
        if (!query.exec("CREATE TABLE IF NOT EXISTS track_gain_v1 (path TEXT PRIMARY KEY NOT NULL, "
                        "size INTEGER NOT NULL, modified_ms INTEGER NOT NULL, gain REAL NOT NULL)") ||
            !query.prepare("INSERT OR REPLACE INTO track_gain_v1 VALUES (?, ?, ?, ?)"))
            return;
        query.addBindValue(key.path);
        query.addBindValue(key.size);
        query.addBindValue(key.modifiedMs);
        query.addBindValue(gain);
        query.exec();
    });
}

QString okj::clearTrackGainCache(const QString &libraryPath)
{
    const auto path = cachePath(libraryPath);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QFile file(path);
        if (!file.remove())
            return QCoreApplication::translate("TrackGainCache", "Could not delete the audio normalization cache: %1")
                .arg(file.errorString());
    }
    ++cacheGeneration;
    return {};
}

quint64 okj::trackGainCacheGeneration()
{
    return cacheGeneration;
}
