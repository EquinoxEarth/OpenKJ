#ifndef MEDIAINITIALIZATION_H
#define MEDIAINITIALIZATION_H

#include <QFuture>
#include <QString>

struct MediaInitializationResult
{
    bool success = false;
    QString error;
    qint64 elapsedMs = 0;
};

// Initialize the media library before constructing any playback backends.
// Pipeline creation and all Qt widgets remain on the GUI thread.
QFuture<MediaInitializationResult> initializeMediaAsync();

#endif // MEDIAINITIALIZATION_H
