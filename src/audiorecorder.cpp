#include "audiorecorder.h"
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include "localfilename.h"
#include <QTemporaryFile>
#include <utility>
#include <QMediaFormat>
#include <spdlog/spdlog.h>
#include <QCoreApplication>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif

namespace {
void requestMicrophonePermission(QObject *context, std::function<void(bool)> finished)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const QMicrophonePermission permission;
    const auto status = qApp->checkPermission(permission);
    if (status == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, context, [finished = std::move(finished)](const QPermission &reply) {
            finished(reply.status() == Qt::PermissionStatus::Granted);
        });
    } else {
        finished(status == Qt::PermissionStatus::Granted);
    }
#else
    Q_UNUSED(context);
    finished(true);
#endif
}

GstElement *createMicrophone(const QString &selection)
{
    if (selection.isEmpty() || selection == "undefined" || selection == "System default") {
#ifdef Q_OS_MACOS
        return gst_element_factory_make("osxaudiosrc", nullptr);
#else
        return gst_element_factory_make("autoaudiosrc", nullptr);
#endif
    }

    auto *monitor = gst_device_monitor_new();
    gst_device_monitor_add_filter(monitor, "Audio/Source", nullptr);
    auto *devices = gst_device_monitor_get_devices(monitor);
    GstElement *source = nullptr;
    for (auto *entry = devices; entry && !source; entry = entry->next) {
        auto *device = GST_DEVICE(entry->data);
        auto *name = gst_device_get_display_name(device);
        if (QString::fromUtf8(name) == selection)
            source = gst_device_create_element(device, nullptr);
        g_free(name);
    }
    g_list_free_full(devices, gst_object_unref);
    gst_object_unref(monitor);
    return source;
}
}

AudioRecorder::AudioRecorder(QObject *parent, PermissionRequest requestPermission)
    : QObject(parent),
      m_requestPermission(requestPermission ? std::move(requestPermission) : requestMicrophonePermission)
{
    logger = spdlog::get("logger");
    if (!logger) {
        logger = spdlog::default_logger();
    }
    m_startDateTime = QDateTime::currentDateTime().toString("yyyy-MM-dd-hhmm");
    // Settings dialogs also construct this object for device/codec lists.
    // Leave capture hardware inactive until recording is explicitly requested.
}

void AudioRecorder::ensureRecorder() {
    if (m_recorder)
        return;
    logger->info("{} Initializing AudioRecorder instance using Qt6 Multimedia", m_loggingPrefix);
    m_captureSession = new QMediaCaptureSession(this);
    m_audioInput = new QAudioInput(this);
    m_recorder = new QMediaRecorder(this);
    connect(m_recorder, &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error, const QString &message) {
        reportError(message.isEmpty() ? tr("The audio recorder could not capture or save the recording.") : message);
    });

    m_captureSession->setAudioInput(m_audioInput);
    m_captureSession->setRecorder(m_recorder);

    // Set default quality
    m_recorder->setQuality(QMediaRecorder::HighQuality);
}

AudioRecorder::~AudioRecorder()
{
    stop();
    gst_clear_object(&m_musicPad);
}

void AudioRecorder::generateDeviceList() {
    m_inputDeviceNames.clear();
    m_audioDevices.clear();
    m_inputDeviceNames.append("System default");
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (qApp->checkPermission(QMicrophonePermission{}) != Qt::PermissionStatus::Granted) {
        // Preserve a saved device selection without querying capture hardware.
        const auto saved = m_settings.recordingInput();
        if (!saved.isEmpty() && saved != "undefined" && !m_inputDeviceNames.contains(saved))
            m_inputDeviceNames.append(saved);
        return; // Browsing settings must not request microphone permission.
    }
#endif
    m_audioDevices = QMediaDevices::audioInputs();
    for (const auto &device : m_audioDevices) {
        m_inputDeviceNames.append(device.description());
    }
}

void AudioRecorder::getRecordingSettings() {
    generateDeviceList();
    setInputDevice(m_inputDeviceNames.indexOf(m_settings.recordingInput()));
    
    QString codec = m_settings.recordingCodec();
    int codecIdx = m_codecs.indexOf(codec);
    if (codecIdx == -1) {
        codecIdx = 1; // Default to OGG Vorbis
    }
    setCurrentCodec(codecIdx);
}

QStringList AudioRecorder::getDeviceList() {
    generateDeviceList();
    return m_inputDeviceNames;
}

QStringList AudioRecorder::getCodecs() {
    return m_codecs;
}

QString AudioRecorder::recordingFileBaseName(const QString &singer, const QString &artist, const QString &title,
                                              const QDateTime &started)
{
    return QStringLiteral("%1 - %2 - %3 - %4")
        .arg(okj::safeFilenamePart(singer, tr("Unknown singer")), okj::safeFilenamePart(artist, tr("Unknown artist")),
             okj::safeFilenamePart(title, tr("Unknown title")), started.toString("yyyy-MM-dd-hhmm"));
}

QString AudioRecorder::prepareOutputDirectory()
{
    const auto baseDirectory = m_settings.recordingOutputDir();
    if (baseDirectory.trimmed().isEmpty()) {
        reportError(tr("Choose a recording output folder in Settings."));
        return {};
    }

    const auto directory = QDir(baseDirectory).absoluteFilePath(
        QStringLiteral("Karaoke Recordings/Show Beginning ") + m_startDateTime);
    if (!QDir().mkpath(directory)) {
        reportError(tr("Could not create the recording folder %1.").arg(QDir::toNativeSeparators(directory)));
        return {};
    }

    QTemporaryFile probe(QDir(directory).filePath(QStringLiteral(".openkj-recording-XXXXXX")));
    if (!probe.open()) {
        reportError(tr("Could not write to the recording folder %1 (%2).")
                    .arg(QDir::toNativeSeparators(directory), probe.errorString()));
        return {};
    }
    return directory;
}

QString AudioRecorder::outputFilePath(const QString &directory, const QString &filename) const
{
    const QDir output(directory);
    auto path = output.filePath(filename + m_currentFileExt);
    for (int suffix = 2; QFileInfo::exists(path) || QFileInfo(path).isSymLink(); ++suffix)
        path = output.filePath(QStringLiteral("%1 (%2)%3").arg(filename).arg(suffix).arg(m_currentFileExt));

    return path;
}

void AudioRecorder::setInputDevice(const int inputDeviceId) {
    if (!m_audioInput) return;
    
    if (inputDeviceId <= 0) {
        logger->info("{} Setting input device to system default", m_loggingPrefix);
        m_audioInput->setDevice(QMediaDevices::defaultAudioInput());
    } else if (inputDeviceId > 0 && inputDeviceId <= m_audioDevices.size()) {
        const auto &device = m_audioDevices.at(inputDeviceId - 1);
        logger->info("{} Setting input device to: {}", m_loggingPrefix, device.description().toStdString());
        m_audioInput->setDevice(device);
    }
}

void AudioRecorder::setCurrentCodec(const int value) {
    QMediaFormat format;
    if (value == 0) {
        format.setFileFormat(QMediaFormat::MP3);
        format.setAudioCodec(QMediaFormat::AudioCodec::MP3);
        m_currentFileExt = ".mp3";
    } else if (value == 1) {
        format.setFileFormat(QMediaFormat::Ogg);
        format.setAudioCodec(QMediaFormat::AudioCodec::Vorbis);
        m_currentFileExt = ".ogg";
    } else {
        format.setFileFormat(QMediaFormat::Wave);
        format.setAudioCodec(QMediaFormat::AudioCodec::Wave);
        m_currentFileExt = ".wav";
    }
    if (!m_recorder)
        return;
    m_recorder->setMediaFormat(format);
    logger->info("{} Setting recording format to extension: {}", m_loggingPrefix, m_currentFileExt.toStdString());
}

void AudioRecorder::setMusicPad(GstPad *pad)
{
    gst_clear_object(&m_musicPad);
    m_musicPad = pad;
}

void AudioRecorder::setMixLevels(int mic, int music)
{
    if (m_mix)
        m_mix->setLevels(mic, music);
}

void AudioRecorder::startMix(const QString &directory, const QString &filename)
{
    if (!m_musicPad) {
        reportError(tr("The karaoke audio is unavailable for OpenKJ mix."));
        return;
    }
    if (m_captureSession)
        m_captureSession->setAudioInput(nullptr);
    const auto input = m_settings.recordingInput();
    auto *microphone = createMicrophone(input);
    if (!microphone) {
        reportError(tr("Could not open the selected microphone for OpenKJ mix."));
        return;
    }

    auto format = RecordingMix::Format::Ogg;
    m_currentFileExt = ".ogg";
    const int codec = m_codecs.indexOf(m_settings.recordingCodec());
    if (codec == 0) {
        format = RecordingMix::Format::Mp3;
        m_currentFileExt = ".mp3";
    } else if (codec == 2) {
        format = RecordingMix::Format::Wav;
        m_currentFileExt = ".wav";
    }
    const auto path = outputFilePath(directory, filename);
    auto *mix = new RecordingMix(this);
    connect(mix, &RecordingMix::error, this, &AudioRecorder::reportError);
    connect(mix, &RecordingMix::finished, this, [this, mix] {
        if (m_mix == mix)
            m_mix.clear();
        mix->deleteLater();
    });
    if (!mix->start(microphone, m_musicPad, path, format,
                    m_settings.recordingMicLevel(), m_settings.recordingMusicLevel())) {
        mix->deleteLater();
        return;
    }
    m_mix = mix;
    logger->info("{} Recording OpenKJ mix to {}", m_loggingPrefix, path.toStdString());
}

void AudioRecorder::record(const QString &singer, const QString &artist, const QString &title)
{
    stop();
    if (!m_settings.recordingEnabled())
        return;

    const auto request = m_recordingRequest;
    const auto filename = recordingFileBaseName(singer, artist, title, QDateTime::currentDateTime());
    m_requestPermission(this, [recorder = QPointer<AudioRecorder>(this), request, filename](bool granted) {
        if (!recorder || request != recorder->m_recordingRequest || !recorder->m_settings.recordingEnabled())
            return;
        if (!granted) {
            recorder->reportError(tr("Microphone permission was not granted."));
            return;
        }

        recorder->m_readyFileName = filename;
        recorder->startPendingRecording();
    });
}

void AudioRecorder::startPendingRecording()
{
    if (m_paused || m_readyFileName.isEmpty())
        return;

    const auto filename = std::exchange(m_readyFileName, {});
    if (!m_settings.recordingEnabled())
        return;
    const auto directory = prepareOutputDirectory();
    if (directory.isEmpty())
        return;

    if (m_settings.recordingUsesOpenKjMix()) {
        startMix(directory, filename);
        return;
    }

    ensureRecorder();
    if (!m_captureSession->audioInput())
        m_captureSession->setAudioInput(m_audioInput);
    getRecordingSettings();
    if (m_audioInput->device().isNull()) {
        reportError(tr("No microphone is available for recording."));
        return;
    }

    const auto path = outputFilePath(directory, filename);
    logger->info("{} Capturing to: {}", m_loggingPrefix, path.toStdString());
    m_recorder->setOutputLocation(QUrl::fromLocalFile(path));
    m_recorder->record();
}

void AudioRecorder::reportError(const QString &message)
{
    logger->error("{} {}", m_loggingPrefix, message.toStdString());
    emit recordingError(message);
}

void AudioRecorder::cancelPendingRecording()
{
    ++m_recordingRequest;
    m_readyFileName.clear();
}

void AudioRecorder::stop()
{
    cancelPendingRecording();
    m_paused = false;
    if (m_mix) {
        m_mix->finish();
        m_mix.clear();
    }
    if (!m_recorder)
        return;
    if (m_recorder->recorderState() != QMediaRecorder::StoppedState)
        logger->info("{} Stopping recording", m_loggingPrefix);
    m_recorder->stop();
}

void AudioRecorder::pause()
{
    m_paused = true;
    if (m_mix)
        m_mix->pause();
    if (m_recorder && m_recorder->recorderState() == QMediaRecorder::RecordingState) {
        logger->info("{} Pausing recording", m_loggingPrefix);
        m_recorder->pause();
    }
}

void AudioRecorder::unpause()
{
    m_paused = false;
    if (m_mix)
        m_mix->resume();
    if (m_recorder && m_recorder->recorderState() == QMediaRecorder::PausedState) {
        logger->info("{} Resuming recording", m_loggingPrefix);
        m_recorder->record();
    } else {
        startPendingRecording();
    }
}
