#ifndef OPENKJ_RECORDINGMIX_H
#define OPENKJ_RECORDINGMIX_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <gst/gst.h>

class RecordingMix : public QObject
{
    Q_OBJECT

public:
    enum class Format { Mp3, Ogg, Wav };

    explicit RecordingMix(QObject *parent = nullptr);
    ~RecordingMix() override;

    // Takes ownership of microphone. The music pad remains owned by playback.
    bool start(GstElement *microphone, GstPad *music, const QString &path, Format format,
               int micLevel, int musicLevel);
    void setLevels(int microphone, int music);
    void pause();
    void resume();
    void finish();

signals:
    void error(const QString &message);
    void finished();

private:
    GstElement *m_pipeline{nullptr};
    GstElement *m_microphone{nullptr};
    GstElement *m_musicSource{nullptr};
    GstElement *m_micVolume{nullptr};
    GstElement *m_musicVolume{nullptr};
    GstPad *m_musicPad{nullptr};
    gulong m_probe{0};
    QTimer m_busTimer;
    QElapsedTimer m_finalizing;

    void attachMusic();
    void detachMusic();
    void pollBus();
    void complete(const QString &error = {});
    void closePipeline();
};

#endif
