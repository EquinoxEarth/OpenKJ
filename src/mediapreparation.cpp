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

PreparedKaraokeMedia prepareKaraokePair(const QString &filename,
                                        const std::shared_ptr<std::atomic_bool> &cancelled)
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
        else {
            result.cdgFile = filename;
            result.audioFile = audioPath;
        }
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
