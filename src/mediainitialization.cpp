#include "mediainitialization.h"

#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <gst/gst.h>

QFuture<MediaInitializationResult> initializeMediaAsync()
{
    return QtConcurrent::run([] {
        QElapsedTimer timer;
        timer.start();
        GError *error = nullptr;
        MediaInitializationResult result;
        result.success = gst_init_check(nullptr, nullptr, &error);
        if (!result.success) {
            result.error = error ? QString::fromUtf8(error->message)
                                 : QStringLiteral("Unable to initialize media support.");
        }
        g_clear_error(&error);
        result.elapsedMs = timer.elapsed();
        return result;
    });
}
