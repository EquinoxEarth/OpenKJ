#ifndef LIBRARYNORMALIZATIONSCAN_H
#define LIBRARYNORMALIZATIONSCAN_H

#include <QPromise>
#include <QMutex>
#include <QString>
#include <atomic>
#include <memory>

namespace okj {
struct LibraryNormalizationResult {
    qsizetype saved{}, skipped{}, failed{}, timedOut{}, cacheFailures{};
    QString error;
};

class LibraryNormalizationControl {
public:
    bool isCancelled() const { return cancelled.load(); }
    bool isPaused() const { return paused.load(); }
    void cancel();
    void setPaused(bool value);
    std::shared_ptr<std::atomic_bool> beginTrack();

private:
    QMutex mutex;
    std::atomic_bool cancelled{false};
    std::atomic_bool paused{false};
    std::shared_ptr<std::atomic_bool> track;
};

void scanLibraryTrackGains(QPromise<LibraryNormalizationResult> &promise, QString libraryPath,
                           std::shared_ptr<LibraryNormalizationControl> control, quint64 cacheGeneration);

}

#endif
