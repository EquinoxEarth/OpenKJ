#ifndef AUDIORECORDER_H
#define AUDIORECORDER_H

#include <QObject>
#include <QStringList>
#include <QDateTime>
#include <functional>
#include <QMediaCaptureSession>
#include <QAudioInput>
#include <QMediaRecorder>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QUrl>
#include "settings.h"
#include "recordingmix.h"
#include <QPointer>
#include <spdlog/logger.h>

class AudioRecorder : public QObject
{
    Q_OBJECT

public:
    using PermissionRequest = std::function<void(QObject *, std::function<void(bool)>)>;

private:
    std::shared_ptr<spdlog::logger> logger;
    std::string m_loggingPrefix{"[AudioRecorder]"};
    Settings m_settings;
    QStringList m_inputDeviceNames;
    QStringList m_codecs{"MPEG 2 Layer 3 (mp3)", "OGG Vorbis", "WAV/PCM"};
    QString m_currentFileExt{".ogg"};
    QString m_startDateTime;
    PermissionRequest m_requestPermission;
    quint64 m_recordingRequest{0};
    QString m_readyFileName;
    bool m_paused{false};
    GstPad *m_musicPad{nullptr};
    QPointer<RecordingMix> m_mix;

    // Qt6 Multimedia members
    QMediaCaptureSession *m_captureSession{nullptr};
    QAudioInput *m_audioInput{nullptr};
    QMediaRecorder *m_recorder{nullptr};
    QList<QAudioDevice> m_audioDevices;

    void ensureRecorder();
    void generateDeviceList();
    void getRecordingSettings();
    void startPendingRecording();
    QString prepareOutputDirectory();
    QString outputFilePath(const QString &directory, const QString &filename) const;
    void startMix(const QString &directory, const QString &filename);
    void setInputDevice(int inputDeviceId);
    void setCurrentCodec(int value);
    void reportError(const QString &message);

public:
    explicit AudioRecorder(QObject *parent = nullptr, PermissionRequest requestPermission = {});
    ~AudioRecorder() override;
    QStringList getDeviceList();
    QStringList getCodecs();
    static QString recordingFileBaseName(const QString &singer, const QString &artist, const QString &title,
                                         const QDateTime &started);
    // Takes ownership of the pad reference returned by MediaBackend.
    void setMusicPad(GstPad *pad);
    void setMixLevels(int mic, int music);
    void record(const QString &singer, const QString &artist, const QString &title);
    void cancelPendingRecording();
    void stop();
    void pause();
    void unpause();

signals:
    void recordingError(const QString &message);
};

#endif // AUDIORECORDER_H
