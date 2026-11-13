#ifndef TRACKGAINCACHE_H
#define TRACKGAINCACHE_H

#include <QString>
#include <optional>

namespace okj {
struct TrackGainKey {
    QString path;
    qint64 size{};
    qint64 modifiedMs{};

    static TrackGainKey fromFile(const QString &path);
    bool operator==(const TrackGainKey &) const = default;
};

// Cache operations run on the main thread and use a separate SQLite file.
std::optional<double> cachedTrackGain(const QString &libraryPath, const TrackGainKey &key);
void cacheTrackGain(const QString &libraryPath, const TrackGainKey &key, double gain);
QString clearTrackGainCache(const QString &libraryPath);
quint64 trackGainCacheGeneration();
}

#endif
