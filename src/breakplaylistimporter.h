#ifndef OPENKJ_BREAKPLAYLISTIMPORTER_H
#define OPENKJ_BREAKPLAYLISTIMPORTER_H

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <memory>

struct BreakPlaylistImportRequest {
    QString databasePath, filename, playlistTitle;
    int playlistId = -1; // Existing destination, or create/reuse the named playlist.
};

struct BreakPlaylistImportResult {
    QString error;
    int playlistId = -1;
    qint64 imported = 0, skipped = 0;
    bool cancelled = false;
};
Q_DECLARE_METATYPE(BreakPlaylistImportResult)

class BreakPlaylistImporter : public QObject {
    Q_OBJECT
public:
    explicit BreakPlaylistImporter(QObject *parent = nullptr);
    ~BreakPlaylistImporter() override;
    bool start(const BreakPlaylistImportRequest &request);
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void progressChanged(qint64 processed, qint64 total);
    void finished(const BreakPlaylistImportResult &result);

private:
    struct State {
        std::atomic_bool cancelled{false};
        std::atomic<qint64> processed{0}, total{0};
    };
    static BreakPlaylistImportResult run(const BreakPlaylistImportRequest &request,
                                         const std::shared_ptr<State> &state);
    QFutureWatcher<BreakPlaylistImportResult> m_watcher{this};
    QTimer m_progressTimer{this};
    std::shared_ptr<State> m_state;
    bool m_running = false;
};

#endif
