#ifndef MEDIAPREPARATION_H
#define MEDIAPREPARATION_H

#include "trackgaincache.h"
#include <QTemporaryDir>
#include <QString>
#include <atomic>
#include <memory>

struct PreparedKaraokeMedia {
    std::shared_ptr<QTemporaryDir> directory;
    QString audioFile, cdgFile, error;
    std::optional<okj::TrackGainKey> sourceGainKey;
    std::optional<qint64> cdgLastDrawMs;
    qint64 elapsedMs{0};
};

// Owns all file access and temporary files; safe to call from a worker thread.
PreparedKaraokeMedia prepareKaraokeZip(const QString &filename,
                                     const std::shared_ptr<std::atomic_bool> &cancelled);
PreparedKaraokeMedia prepareKaraokePair(const QString &filename,
                                        const std::shared_ptr<std::atomic_bool> &cancelled, bool stageFiles = true);
qint64 cdgLastDrawPositionMs(const QString &path, const std::atomic_bool *cancelled = nullptr);
PreparedKaraokeMedia prepareKaraokeVideo(const QString &filename,
                                       const std::shared_ptr<std::atomic_bool> &cancelled);

#endif // MEDIAPREPARATION_H
