#ifndef AUDIONORMALIZATION_H
#define AUDIONORMALIZATION_H

#include <QString>
#include <atomic>
#include <optional>

namespace okj {
enum class GainAnalysisStatus { Analyzed, Cancelled, DecodeFailed, TimedOut };
struct GainAnalysisResult {
    GainAnalysisStatus status;
    std::optional<double> gain;
};
bool trackGainAnalyzerAvailable();
GainAnalysisResult analyzeTrackGainResult(const QString &path, const std::atomic_bool &cancelled);

// Run on a worker after GStreamer has been initialized.
std::optional<double> analyzeTrackGain(const QString &path, const std::atomic_bool &cancelled);
}

#endif
