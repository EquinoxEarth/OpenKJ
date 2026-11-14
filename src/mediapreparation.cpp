#include "mediapreparation.h"
#include "mzarchive.h"
#include "okjutil.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

PreparedKaraokeMedia prepareKaraokeZip(const QString &filename,
                                  const std::shared_ptr<std::atomic_bool> &cancelled)
{
    PreparedKaraokeMedia result;
    QElapsedTimer timer;
    timer.start();
    if (cancelled->load())
        return result;
    // Construction, archive access, and extraction all stay on this thread.
    result.sourceGainKey = okj::TrackGainKey::fromFile(filename);
    MzArchive archive(filename);
    if (!archive.checkCDG() || !archive.checkAudio()) {
        result.error = QStringLiteral("Zip file does not contain a valid karaoke track.  CDG or audio file missing or corrupt.");
    } else if (!cancelled->load()) {
        result.directory = std::make_shared<QTemporaryDir>();
        if (!result.directory->isValid()) {
            result.error = QStringLiteral("Failed to create temporary directory.");
        } else {
            const auto audioName = "tmp" + archive.audioExtension();
            if (!archive.extractAudio(result.directory->path(), audioName, cancelled.get()))
                result.error = QStringLiteral("Failed to extract audio file.");
            else if (!cancelled->load() && !archive.extractCdg(result.directory->path(), "tmp.cdg", cancelled.get()))
                result.error = QStringLiteral("Failed to extract CDG file.");
            if (!cancelled->load() && result.error.isEmpty()) {
                result.audioFile = result.directory->filePath(audioName);
                result.cdgFile = result.directory->filePath("tmp.cdg");
                result.cdgLastDrawMs = cdgLastDrawPositionMs(result.cdgFile, cancelled.get());
            }
        }
    }
    if (cancelled->load() || !result.error.isEmpty()) {
        result.directory.reset();
        result.audioFile.clear();
        result.cdgFile.clear();
    }
    result.elapsedMs = timer.elapsed();
    return result;
}

namespace {
bool copyUntilCancelled(const QString &source, const QString &target, const std::atomic_bool &cancelled)
{
    QFile input(source), output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        return false;
    while (!cancelled.load()) {
        char buffer[64 * 1024];
        const auto size = input.read(buffer, sizeof(buffer));
        if (size <= 0)
            return size == 0 && output.flush();
        if (output.write(buffer, size) != size)
            return false;
    }
    return false;
}
}

PreparedKaraokeMedia prepareKaraokePair(const QString &filename,
                                        const std::shared_ptr<std::atomic_bool> &cancelled, bool stageFiles)
{
    PreparedKaraokeMedia result;
    QElapsedTimer timer;
    timer.start();
    if (cancelled->load())
        return result;

    const QFileInfo cdg(filename);
    if (!cdg.isFile() || !cdg.isReadable()) {
        result.error = QStringLiteral("CDG file missing or unreadable.");
    } else if (cdg.size() == 0) {
        result.error = QStringLiteral("CDG file contains no data.");
    } else {
        const auto audioPath = findMatchingAudioFile(filename);
        const QFileInfo audio(audioPath);
        if (!audio.isFile() || !audio.isReadable())
            result.error = QStringLiteral("Audio file missing or unreadable.");
        else if (audio.size() == 0)
            result.error = QStringLiteral("Audio file contains no data.");
        else if (!stageFiles) {
            result.cdgFile = filename;
            result.audioFile = audioPath;
        } else if (!cancelled->load()) {
            result.sourceGainKey = okj::TrackGainKey::fromFile(audioPath);
            result.directory = std::make_shared<QTemporaryDir>();
            if (!result.directory->isValid()) {
                result.error = QStringLiteral("Failed to create temporary directory.");
            } else {
                result.audioFile = result.directory->filePath("song." + audio.suffix());
                result.cdgFile = result.directory->filePath("song.cdg");
                if (!copyUntilCancelled(audioPath, result.audioFile, *cancelled) ||
                    !copyUntilCancelled(filename, result.cdgFile, *cancelled))
                    result.error = QStringLiteral("Failed to copy the CDG or its audio file.");
                else
                    result.cdgLastDrawMs = cdgLastDrawPositionMs(result.cdgFile, cancelled.get());
            }
        }
    }
    if (cancelled->load() || !result.error.isEmpty()) {
        result.directory.reset();
        result.audioFile.clear();
        result.cdgFile.clear();
    }
    result.elapsedMs = timer.elapsed();
    return result;
}

PreparedKaraokeMedia prepareKaraokeVideo(const QString &filename,
                                       const std::shared_ptr<std::atomic_bool> &cancelled)
{
    PreparedKaraokeMedia result;
    QElapsedTimer timer;
    timer.start();
    if (cancelled->load())
        return result;
    const QFileInfo video(filename);
    QFile source(video.absoluteFilePath());
    if (!video.isFile()) {
        result.error = QStringLiteral("Video file missing or unreadable.");
    } else if (!source.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Unable to open video file: ") + source.errorString();
    } else if (source.size() == 0) {
        result.error = QStringLiteral("Video file contains no data.");
    } else if (!cancelled->load()) {
        result.audioFile = source.fileName();
    }
    result.elapsedMs = timer.elapsed();
    return result;
}

qint64 cdgLastDrawPositionMs(const QString &path, const std::atomic_bool *cancelled)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;

    constexpr qint64 packetSize = 24;
    constexpr qint64 chunkPackets = 4096;
    qint64 end = file.size() / packetSize;
    while (end > 0 && (!cancelled || !cancelled->load())) {
        const qint64 start = qMax(qint64(0), end - chunkPackets);
        if (!file.seek(start * packetSize))
            return 0;
        const auto bytes = file.read((end - start) * packetSize);
        if (bytes.size() != (end - start) * packetSize)
            return 0;
        for (qint64 packet = end - start; packet > 0;) {
            const auto offset = --packet * packetSize;
            const int instruction = bytes[offset + 1] & 0x3f;
            if ((bytes[offset] & 0x3f) == 9 &&
                (instruction == 6 || instruction == 38 || instruction == 20 ||
                 instruction == 24 || instruction == 1))
                return ((start + packet) * 10) / 3;
        }
        end = start;
    }
    return 0;
}
