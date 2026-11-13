#include "trackgaincache.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <cmath>

namespace {
QMutex cacheMutex;
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

constexpr auto createCache = "CREATE TABLE IF NOT EXISTS track_gain_v1 (path TEXT PRIMARY KEY NOT NULL, "
                             "size INTEGER NOT NULL, modified_ms INTEGER NOT NULL, gain REAL NOT NULL)";

constexpr auto selectGain = "SELECT gain FROM track_gain_v1 WHERE path = ? AND size = ? AND modified_ms = ?";

std::optional<double> lookupGain(QSqlQuery &query, const okj::TrackGainKey &key)
{
    query.bindValue(0, key.path);
    query.bindValue(1, key.size);
    query.bindValue(2, key.modifiedMs);
    std::optional<double> result;
    if (query.exec() && query.next()) {
        bool ok = false;
        const auto gain = query.value(0).toDouble(&ok);
        if (ok && validGain(gain))
            result = gain;
    }
    query.finish();
    return result;
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
    QMutexLocker lock(&cacheMutex);
    withCache(cachePath(libraryPath), true, [&](QSqlQuery &query) {
        if (query.prepare(selectGain))
            result = lookupGain(query, key);
    });
    return result;
}

QList<okj::TrackGainKey> okj::uncachedTrackGains(const QString &libraryPath, const QList<TrackGainKey> &keys)
{
    auto pending = keys;
    QMutexLocker lock(&cacheMutex);
    withCache(cachePath(libraryPath), true, [&](QSqlQuery &query) {
        if (query.prepare(selectGain))
            pending.removeIf([&](const auto &key) { return lookupGain(query, key).has_value(); });
    });
    return pending;
}

bool okj::prepareTrackGainCache(const QString &libraryPath, quint64 generation)
{
    QMutexLocker lock(&cacheMutex);
    if (generation != cacheGeneration)
        return false;
    bool ready = false;
    withCache(cachePath(libraryPath), false, [&](QSqlQuery &query) {
        // Take a write lock briefly so a read-only cache fails before decoding.
        if (!query.exec("BEGIN IMMEDIATE"))
            return;
        ready = query.exec(createCache) && query.exec("COMMIT");
        if (!ready)
            query.exec("ROLLBACK");
    });
    return ready;
}

okj::GainCacheWrite okj::cacheTrackGain(const QString &libraryPath, const TrackGainKey &key, double gain,
                                        std::optional<quint64> generation)
{
    if (!validGain(gain) || !isCurrent(key))
        return GainCacheWrite::Stale;
    QMutexLocker lock(&cacheMutex);
    if (generation && *generation != cacheGeneration)
        return GainCacheWrite::Stale;
    bool saved = false;
    withCache(cachePath(libraryPath), false, [&](QSqlQuery &query) {
        if (!query.exec(createCache) ||
            !query.prepare("INSERT OR REPLACE INTO track_gain_v1 VALUES (?, ?, ?, ?)"))
            return;
        query.addBindValue(key.path);
        query.addBindValue(key.size);
        query.addBindValue(key.modifiedMs);
        query.addBindValue(gain);
        saved = query.exec();
    });
    return saved ? GainCacheWrite::Saved : GainCacheWrite::Failed;
}

QString okj::clearTrackGainCache(const QString &libraryPath)
{
    QMutexLocker lock(&cacheMutex);
    ++cacheGeneration;
    const auto path = cachePath(libraryPath);
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QFile file(path);
        if (!file.remove())
            return QCoreApplication::translate("TrackGainCache", "Could not delete the audio normalization cache: %1")
                .arg(file.errorString());
    }
    return {};
}

quint64 okj::trackGainCacheGeneration()
{
    QMutexLocker lock(&cacheMutex);
    return cacheGeneration;
}
