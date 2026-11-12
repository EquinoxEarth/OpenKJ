#ifndef DLGVIDEOPREVIEW_H
#define DLGVIDEOPREVIEW_H

#include <QDialog>
#include <atomic>
#include <memory>
#include <optional>
#include "mediabackend.h"

namespace Ui {
class DlgVideoPreview;
}

class DlgVideoPreview : public QDialog
{
    Q_OBJECT

public:
    explicit DlgVideoPreview(QString mediaFilePath, QWidget *parent = nullptr,
                             const QStringList &activeOutputs = {});
    ~DlgVideoPreview() override;
    void validateAudioOutput(const QStringList &activeOutputs);
    // Used by torture tests to bound how long a preview stays open.
    void setPlaybackTimeLimit(int playSecs);

public slots:
    void done(int result) override;

private:
    std::unique_ptr<Ui::DlgVideoPreview> ui;
    const QString m_mediaFilename;
    std::shared_ptr<std::atomic_bool> m_cancelled = std::make_shared<std::atomic_bool>(false);
    // Destroy the backend before releasing any files it can still be using.
    std::shared_ptr<QTemporaryDir> m_preparedDirectory;
    std::optional<int> m_closeResult;
    bool m_mediaLoaded{false};
    bool m_failed{false};
    MediaBackend m_mediaBackend{this, "PREVIEW", MediaBackend::VideoPreview};

    void updateMuteButton(bool muted);
    void showPlaybackError(const QString &error);
};

#endif // DLGVIDEOPREVIEW_H
