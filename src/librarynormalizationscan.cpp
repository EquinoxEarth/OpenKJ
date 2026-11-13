#include "librarynormalizationscan.h"

#include "audionormalization.h"
#include "mzarchive.h"
#include "okjutil.h"
#include "trackgaincache.h"

#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

namespace {
QStringList libraryPaths(const QString &path, const std::atomic_bool &cancelled, QString &error)
{
    const auto name = "normalization-" + QUuid::createUuid().toString();
    const auto removeConnection = qScopeGuard([&] { QSqlDatabase::removeDatabase(name); });
    auto database = QSqlDatabase::addDatabase("QSQLITE", name);
    database.setDatabaseName(path);
    database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=250");
    if (!database.open()) {
        error = database.lastError().text();
        return {};
    }

    QStringList paths;
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (query.exec("SELECT path FROM dbSongs")) {
        while (!cancelled.load() && query.next())
            paths.append(query.value(0).toString());
    }
    error = query.lastError().text();
    return paths;
}

std::optional<double> analyzeLibraryTrack(const okj::TrackGainKey &key, const std::atomic_bool &cancelled)
{
    if (!key.path.endsWith(".zip", Qt::CaseInsensitive))
        return okj::analyzeTrackGain(key.path, cancelled);

    MzArchive archive(key.path);
    if (!archive.isValidKaraokeFile(false) || cancelled.load())
        return {};
    QTemporaryDir directory;
    const auto audio = "audio" + archive.audioExtension();
    if (!directory.isValid() || !archive.extractAudio(directory.path(), audio, &cancelled, false))
        return {};
    return okj::analyzeTrackGain(directory.filePath(audio), cancelled);
}
}

void okj::scanLibraryTrackGains(QPromise<QString> &promise, QString libraryPath,
                                std::shared_ptr<std::atomic_bool> cancelled, quint64 cacheGeneration)
{
    QString error;
    const auto paths = libraryPaths(libraryPath, *cancelled, error);
    if (!error.isEmpty()) {
        promise.addResult(error);
        return;
    }

    // Release the library read before touching files. Query the cache in small
    // batches so a large library cannot hold up playback's cache lookups.
    QList<TrackGainKey> pending, batch;
    constexpr int batchSize = 64;
    for (const auto &path : paths) {
        if (cancelled->load())
            return;
        const auto source = TrackGainKey::fromFile(path);
        if (source.path.isEmpty())
            continue;
        const auto key = path.endsWith(".cdg", Qt::CaseInsensitive)
            ? TrackGainKey::fromFile(findMatchingAudioFile(path)) : source;
        if (!key.path.isEmpty())
            batch.append(key);
        if (batch.size() == batchSize) {
            pending.append(uncachedTrackGains(libraryPath, batch));
            batch.clear();
        }
    }
    if (!batch.isEmpty())
        pending.append(uncachedTrackGains(libraryPath, batch));

    promise.setProgressRange(0, static_cast<int>(pending.size()));
    int processed = 0;
    for (const auto &key : pending) {
        if (cancelled->load())
            return;
        // Playback may have filled the cache after the scan built its list.
        if (!cachedTrackGain(libraryPath, key)) {
            const auto gain = analyzeLibraryTrack(key, *cancelled);
            if (gain && !cancelled->load())
                cacheTrackGain(libraryPath, key, *gain, cacheGeneration);
        }
        promise.setProgressValue(++processed);
    }
}
