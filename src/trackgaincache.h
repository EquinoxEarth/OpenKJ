#ifndef TRACKGAINCACHE_H
#define TRACKGAINCACHE_H

#include <QString>
#include <QList>
#include <optional>

namespace okj {
struct TrackGainKey {
    QString path;
    qint64 size{};
    qint64 modifiedMs{};

    static TrackGainKey fromFile(const QString &path);
    bool operator==(const TrackGainKey &) const = default;
};

// Connections are opened and closed on the calling thread.
std::optional<double> cachedTrackGain(const QString &libraryPath, const TrackGainKey &key);
QList<TrackGainKey> uncachedTrackGains(const QString &libraryPath, const QList<TrackGainKey> &keys);
void cacheTrackGain(const QString &libraryPath, const TrackGainKey &key, double gain,
                    std::optional<quint64> generation = std::nullopt);
QString clearTrackGainCache(const QString &libraryPath);
quint64 trackGainCacheGeneration();
}

#endif
