#include "dlgvideopreview.h"
#include "mediapreparation.h"
#include "ui_dlgvideopreview.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <chrono>
#include <utility>

DlgVideoPreview::DlgVideoPreview(QString mediaFilePath, QWidget *parent, const QStringList &activeOutputs)
    : QDialog(parent), ui(new Ui::DlgVideoPreview), m_mediaFilename(std::move(mediaFilePath))
{
    ui->setupUi(this);
    connect(ui->pushButtonClose, &QPushButton::clicked, this, &QWidget::close);
    m_mediaBackend.setVideoOutputWidgets({ui->videoDisplay});
    m_mediaBackend.setUseSilenceDetection(false);
    Settings settings;
    const auto output = settings.previewAudioOutputDevice();
    if (!output.isEmpty() && MediaBackend::getPreviewOutputDevices(activeOutputs).contains(output))
        m_mediaBackend.setAudioOutputDevice(output);
    const bool audioAvailable = m_mediaBackend.hasPreviewAudioOutput();
    ui->toolButtonMute->setEnabled(audioAvailable);
    ui->sliderVolume->setEnabled(audioAvailable);
    ui->sliderVolume->setValue(m_mediaBackend.getVolume());
    connect(ui->sliderVolume, &QSlider::valueChanged, &m_mediaBackend, &MediaBackend::setVolume);
    connect(&m_mediaBackend, &MediaBackend::mutedChanged, this, &DlgVideoPreview::updateMuteButton);
    connect(ui->toolButtonMute, &QToolButton::clicked, this, [this] {
        if (ui->toolButtonMute->isEnabled())
            m_mediaBackend.setMuted(!m_mediaBackend.isMuted());
    });
    m_mediaBackend.setMuted(!audioAvailable || settings.previewStartsMuted());
    if (audioAvailable) {
        auto *devices = new QMediaDevices(this);
        connect(devices, &QMediaDevices::audioOutputsChanged, this, [this, activeOutputs] {
            validateAudioOutput(activeOutputs);
        });
    }
    connect(&m_mediaBackend, &MediaBackend::audioError, this, &DlgVideoPreview::showPlaybackError);
    connect(&m_mediaBackend, &MediaBackend::stopCompleted, this, [this] {
        if (m_closeResult && m_mediaBackend.isPlaybackStopped()) {
            const int result = *m_closeResult;
            m_closeResult.reset();
            QDialog::done(result);
        }
    }, Qt::QueuedConnection);

    auto *watcher = new QFutureWatcher<PreparedKaraokeMedia>(this);
    connect(watcher, &QFutureWatcher<PreparedKaraokeMedia>::finished, this, [this, watcher] {
        watcher->deleteLater();
        if (m_cancelled->load())
            return;
        auto prepared = watcher->future().takeResult();
        if (!prepared.error.isEmpty()) {
            showPlaybackError(prepared.error);
            return;
        }

        m_preparedDirectory = std::move(prepared.directory);
        m_mediaLoaded = true;
        if (prepared.cdgFile.isEmpty())
            m_mediaBackend.setMedia(prepared.audioFile);
        else
            m_mediaBackend.setMediaCdg(prepared.cdgFile, prepared.audioFile);
        if (!m_failed)
            m_mediaBackend.play();
    });
    watcher->setFuture(QtConcurrent::run([filename = m_mediaFilename, cancelled = m_cancelled] {
        if (filename.endsWith(".zip", Qt::CaseInsensitive))
            return prepareKaraokeZip(filename, cancelled);
        if (filename.endsWith(".cdg", Qt::CaseInsensitive))
            return prepareKaraokePair(filename, cancelled);

        PreparedKaraokeMedia result;
        if (cancelled->load())
            return result;
        const QFileInfo video(filename);
        if (!video.isFile() || !video.isReadable())
            result.error = tr("Video file missing or unreadable.");
        else if (video.size() == 0)
            result.error = tr("Video file contains no data.");
        else
            result.audioFile = video.absoluteFilePath();
        return result;
    }));
}

DlgVideoPreview::~DlgVideoPreview()
{
    m_cancelled->store(true);
}

void DlgVideoPreview::validateAudioOutput(const QStringList &activeOutputs)
{
    if (!ui->toolButtonMute->isEnabled())
        return;
    Settings settings;
    const auto output = m_mediaBackend.activeAudioOutputDevice();
    if (output == settings.previewAudioOutputDevice() &&
        MediaBackend::getPreviewOutputDevices(activeOutputs).contains(output))
        return;
    ui->toolButtonMute->setEnabled(false);
    ui->sliderVolume->setEnabled(false);
    m_mediaBackend.setMuted(true);
    // Closing also releases the sink so the new route can use the device.
    close();
}

void DlgVideoPreview::updateMuteButton(bool muted)
{
    auto icon = style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume)
                    .pixmap(ui->toolButtonMute->iconSize(), devicePixelRatioF());
    if (!icon.isNull()) {
        QPainter painter(&icon);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRectF(QPointF(), icon.deviceIndependentSize()), Qt::white);
    }
    ui->toolButtonMute->setIcon(QIcon(icon));
    const auto action = muted ? tr("Unmute preview") : tr("Mute preview");
    ui->toolButtonMute->setAccessibleName(action);
    ui->toolButtonMute->setToolTip(ui->toolButtonMute->isEnabled()
        ? action : tr("Select a separate output in Settings → Audio → Preview, then reopen the preview."));
}

void DlgVideoPreview::done(int result)
{
    if (m_cancelled->exchange(true))
        return;
    if (!m_mediaLoaded) {
        QDialog::done(result);
        return;
    }
    // Keep the dialog and prepared files alive until native playback has stopped.
    m_closeResult = result;
    m_mediaBackend.rawStop();
}

void DlgVideoPreview::showPlaybackError(const QString &error)
{
    if (m_cancelled->load() || m_failed)
        return;
    m_failed = true;
    auto *message = new QMessageBox(QMessageBox::Warning, tr("Video Preview"),
                                    tr("Could not preview this song."), QMessageBox::Ok, this);
    message->setTextFormat(Qt::PlainText);
    message->setInformativeText(error + QStringLiteral("\n\n") + m_mediaFilename);
    message->setAttribute(Qt::WA_DeleteOnClose);
    connect(message, &QMessageBox::finished, this, &QWidget::close);
    message->open();
}

void DlgVideoPreview::setPlaybackTimeLimit(int playSecs)
{
    if (playSecs > 0)
        QTimer::singleShot(std::chrono::seconds(playSecs), this, &QWidget::close);
}
