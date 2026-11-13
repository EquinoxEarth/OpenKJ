#ifndef AUDIONORMALIZATION_H
#define AUDIONORMALIZATION_H

#include <QString>
#include <atomic>
#include <optional>

namespace okj {
// Run on a worker after GStreamer has been initialized.
std::optional<double> analyzeTrackGain(const QString &path, const std::atomic_bool &cancelled);
}

#endif
