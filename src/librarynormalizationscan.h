#ifndef LIBRARYNORMALIZATIONSCAN_H
#define LIBRARYNORMALIZATIONSCAN_H

#include <QPromise>
#include <QString>
#include <atomic>
#include <memory>

namespace okj {
// Runs on a worker. The result is empty on success or cancellation.
void scanLibraryTrackGains(QPromise<QString> &promise, QString libraryPath,
                           std::shared_ptr<std::atomic_bool> cancelled, quint64 cacheGeneration);
}

#endif
