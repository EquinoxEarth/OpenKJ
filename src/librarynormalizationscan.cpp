#include "librarynormalizationscan.h"

#include "audionormalization.h"
#include "mzarchive.h"
#include "okjutil.h"
#include "trackgaincache.h"

#include <QScopeGuard>
#include <QThread>
#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

namespace {
QStringList libraryPaths(const QString &path, const okj::LibraryNormalizationControl &control, QString &error)
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
        while (!control.isCancelled() && query.next())
            paths.append(query.value(0).toString());
    }
    error = query.lastError().text();
    return paths;
}

okj::GainAnalysisResult analyzeLibraryTrack(const okj::TrackGainKey &key, const std::atomic_bool &cancelled)
{
    if (!key.path.endsWith(".zip", Qt::CaseInsensitive))
        return okj::analyzeTrackGainResult(key.path, cancelled);

    MzArchive archive(key.path);
    if (!archive.isValidKaraokeFile(false))
        return {okj::GainAnalysisStatus::DecodeFailed, {}};
    QTemporaryDir directory;
    const auto audio = "audio" + archive.audioExtension();
    if (!directory.isValid() || !archive.extractAudio(directory.path(), audio, &cancelled, false))
        return {okj::GainAnalysisStatus::DecodeFailed, {}};
    return okj::analyzeTrackGainResult(directory.filePath(audio), cancelled);
}
}

void okj::LibraryNormalizationControl::cancel()
{
    QMutexLocker lock(&mutex);
    cancelled.store(true);
    if (track)
        track->store(true);
}

void okj::LibraryNormalizationControl::setPaused(bool value)
{
    QMutexLocker lock(&mutex);
    paused.store(value);
    if (value && track)
        track->store(true);
}

std::shared_ptr<std::atomic_bool> okj::LibraryNormalizationControl::beginTrack()
{
    QMutexLocker lock(&mutex);
    if (paused.load() || cancelled.load())
        return {};
    track = std::make_shared<std::atomic_bool>(false);
    return track;
}

void okj::scanLibraryTrackGains(QPromise<LibraryNormalizationResult> &promise, QString libraryPath,
                                std::shared_ptr<LibraryNormalizationControl> control, quint64 cacheGeneration)
{
    const auto waitForKaraoke = [&] {
        while (control->isPaused() && !control->isCancelled())
            QThread::msleep(50);
        return !control->isCancelled();
    };
    LibraryNormalizationResult result;
    if (!waitForKaraoke())
        return;
    const auto paths = libraryPaths(libraryPath, *control, result.error);
    const auto tr = [](const char *message) { return QCoreApplication::translate("LibraryNormalization", message); };
    if (result.error.isEmpty() && !paths.isEmpty()) {
        if (!trackGainAnalyzerAvailable())
            result.error = tr("The ReplayGain analyzer is unavailable.");
        else if (!prepareTrackGainCache(libraryPath, cacheGeneration))
            result.error = tr("The audio normalization cache could not be opened for writing.");
    }
    if (!result.error.isEmpty()) {
        promise.addResult(result);
        return;
    }

    QList<TrackGainKey> pending, batch;
    constexpr int batchSize = 64;
    const auto collect = [&] {
        const auto uncached = uncachedTrackGains(libraryPath, batch);
        result.skipped += batch.size() - uncached.size();
        pending.append(uncached);
        batch.clear();
    };
    for (const auto &path : paths) {
        if (!waitForKaraoke())
            return;
        const auto source = TrackGainKey::fromFile(path);
        const auto key = !source.path.isEmpty() && path.endsWith(".cdg", Qt::CaseInsensitive)
            ? TrackGainKey::fromFile(findMatchingAudioFile(path)) : source;
        if (key.path.isEmpty())
            ++result.skipped;
        else
            batch.append(key);
        if (batch.size() == batchSize)
            collect();
    }
    if (!batch.isEmpty())
        collect();

    promise.setProgressRange(0, static_cast<int>(pending.size()));
    int processed = 0;
    while (processed < pending.size()) {
        const auto &key = pending[processed];
        if (!waitForKaraoke() || cacheGeneration != trackGainCacheGeneration())
            return;
        if (cachedTrackGain(libraryPath, key)) {
            ++result.skipped;
        } else if (TrackGainKey::fromFile(key.path) != key) {
            ++result.skipped;
        } else {
            const auto interrupted = control->beginTrack();
            if (!interrupted)
                continue;
            const auto analysis = analyzeLibraryTrack(key, *interrupted);
            if (interrupted->load())
                continue;
            if (analysis.gain) {
                switch (cacheTrackGain(libraryPath, key, *analysis.gain, cacheGeneration)) {
                case GainCacheWrite::Saved: ++result.saved; break;
                case GainCacheWrite::Stale: ++result.skipped; break;
                case GainCacheWrite::Failed: ++result.cacheFailures; break;
                }
            } else if (analysis.status == GainAnalysisStatus::TimedOut) {
                ++result.timedOut;
            } else {
                ++result.failed;
            }
        }
        promise.setProgressValue(++processed);
    }
    promise.addResult(result);
}
