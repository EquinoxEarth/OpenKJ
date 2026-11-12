#include "mediabackend.h"
#include "audiofader.h"
#include "dlgvideopreview.h"
#include "xbrz.h"
#include "recordingmix.h"
#include <QtEndian>
#include <cmath>
#include <numbers>
#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QSlider>
#include <QSignalSpy>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUuid>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <spdlog/sinks/callback_sink.h>
#include <zip.h>

std::ostream &operator<<(std::ostream &stream, const QString &text)
{
    return stream << text.toStdString();
}

// Virtual devices exercise routing without opening the host's audio hardware.
struct TestAudioDevice { GstDevice parent; };
struct TestAudioDeviceClass { GstDeviceClass parent; };
G_DEFINE_TYPE(TestAudioDevice, test_audio_device, GST_TYPE_DEVICE)
static QStringList created;
static std::atomic_int silentPreviewBuffers{0};
static std::atomic_int audiblePreviewBuffers{0};

static void capturePreviewAudio(GstElement *, GstBuffer *buffer, GstPad *, gpointer)
{
    GstMapInfo data;
    if (!gst_buffer_map(buffer, &data, GST_MAP_READ))
        return;
    const bool silent = std::all_of(data.data, data.data + data.size, [](guint8 byte) { return byte == 0; });
    gst_buffer_unmap(buffer, &data);
    ++(silent ? silentPreviewBuffers : audiblePreviewBuffers);
}

static GstElement *createOutput(GstDevice *device, const gchar *name)
{
    auto *displayName = gst_device_get_display_name(device);
    created.append(QString::fromUtf8(displayName));
    g_free(displayName);
    auto *sink = gst_element_factory_make("fakesink", name);
    g_object_set(sink, "sync", TRUE, nullptr);
    if (created.last() == "Test USB") {
        g_object_set(sink, "signal-handoffs", TRUE, nullptr);
        g_signal_connect(sink, "handoff", G_CALLBACK(capturePreviewAudio), nullptr);
    }
    return sink;
}
static void test_audio_device_class_init(TestAudioDeviceClass *klass)
{
    GST_DEVICE_CLASS(klass)->create_element = createOutput;
}
static void test_audio_device_init(TestAudioDevice *) {}

struct TestAudioProvider { GstDeviceProvider parent; };
struct TestAudioProviderClass { GstDeviceProviderClass parent; };
G_DEFINE_TYPE(TestAudioProvider, test_audio_provider, GST_TYPE_DEVICE_PROVIDER)
static bool connected = true;
static GList *probeOutputs(GstDeviceProvider *)
{
    GList *devices = nullptr;
    auto *caps = gst_caps_from_string("audio/x-raw");
    if (connected) {
        for (const auto *name : {"Test Speakers", "Test USB"}) {
            auto *properties = gst_structure_new("test-output", "is-default", G_TYPE_BOOLEAN,
                                                 qstrcmp(name, "Test Speakers") == 0, nullptr);
            auto *device = GST_DEVICE(g_object_new(test_audio_device_get_type(),
                "display-name", name, "device-class", "Audio/Sink", "caps", caps,
                "properties", properties, nullptr));
            gst_structure_free(properties);
            devices = g_list_append(devices, gst_object_ref_sink(device));
        }
    }
    auto *input = GST_DEVICE(g_object_new(test_audio_device_get_type(),
        "display-name", "Test Microphone", "device-class", "Audio/Source", "caps", caps, nullptr));
    gst_caps_unref(caps);
    return g_list_append(devices, gst_object_ref_sink(input));
}
static void test_audio_provider_class_init(TestAudioProviderClass *klass)
{
    auto *provider = GST_DEVICE_PROVIDER_CLASS(klass);
    provider->probe = probeOutputs;
    gst_device_provider_class_set_static_metadata(provider, "Test outputs", "Audio/Sink/Source",
                                                   "Isolated output routing test", "OpenKJ");
}
static void test_audio_provider_init(TestAudioProvider *) {}
static void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<typename Predicate>
static void waitFor(Predicate predicate, int timeout, const char *message)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout)
        QTest::qWait(20);
    require(predicate(), message);
}

static void checkSettingsOwnershipAndPersistence()
{
    require(QStandardPaths::isTestModeEnabled(), "Settings checks require isolated test-mode paths");
    QPointer<QSettings> storage;
    QString path;
    QSettings::Format format = QSettings::IniFormat;
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto key = "tests/settingsOwnership/" + token;
    {
        Settings owner;
        storage = owner.findChild<QSettings *>(QString(), Qt::FindDirectChildrenOnly);
        require(storage && storage->parent() == &owner, "Settings must own its QSettings object");
        path = storage->fileName();
        format = storage->format();
        storage->setValue(key, token);
        storage->sync();
        require(storage->status() == QSettings::NoError, "Test settings must be writable");
    }
    require(storage.isNull(), "Destroying Settings must release its QSettings object");
    QSettings reloaded(path, format);
    const auto cleanup = qScopeGuard([&] { reloaded.remove(key); reloaded.sync(); });
    require(reloaded.value(key).toString() == token, "Settings must survive destruction and reopening");
    QFile saved(path);
    require(saved.open(QIODevice::ReadOnly) && saved.readAll().contains(token.toUtf8()),
            "Settings must actually be written to disk");
}

static void checkVideoSettingsReuse()
{
    VideoDisplay view;
    view.resize(600, 432);
    auto *owner = view.findChild<Settings *>(QString(), Qt::FindDirectChildrenOnly);
    require(owner, "Each video view must retain one Settings instance");
    QPointer<QSettings> storage = owner->findChild<QSettings *>(QString(), Qt::FindDirectChildrenOnly);
    require(storage, "The video view's settings storage must be owned");
    Settings controls;
    const bool original = controls.cdgPrescalingEnabled();
    const auto restore = qScopeGuard([&] { controls.setCdgPrescalingEnabled(original); });
    QImage frame(300, 216, QImage::Format_RGB888);
    for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x)
            frame.setPixelColor(x, y, x < y ? Qt::white : Qt::black);
    view.updateFrame(frame);
    controls.setCdgPrescalingEnabled(false);
    const auto ordinary = view.grab().toImage();
    controls.setCdgPrescalingEnabled(true);
    const auto prescaled = view.grab().toImage();
    controls.setCdgPrescalingEnabled(false);
    require(ordinary == view.grab().toImage(), "Prescaling changes must remain reversible on an existing view");
    require(ordinary != prescaled, "An existing view must observe a changed CDG prescaling preference");
    for (int paint = 0; paint < 100; ++paint)
        view.grab();
    require(view.findChildren<Settings *>(QString(), Qt::FindDirectChildrenOnly).size() == 1 && storage,
            "Repainting must retain the same settings owner and storage");
}

static void checkSharedRendering()
{
    QImage image(300, 216, QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, x < y ? Qt::white : Qt::blue);

    auto frame = std::make_shared<VideoDisplayFrame>(image);
    const auto source = image.convertToFormat(QImage::Format_ARGB32);
    QImage enlarged(600, 432, QImage::Format_ARGB32);
    xbrz::scale(2, reinterpret_cast<const uint32_t *>(source.constBits()),
                reinterpret_cast<uint32_t *>(enlarged.bits()), source.width(), source.height(),
                xbrz::ColorFormat::ARGB);
    const QSize bounds(640, 600);
    const auto expected = enlarged.scaled(image.size().scaled(bounds, Qt::KeepAspectRatio),
                                          Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto scaled = frame->scaled(bounds, true);
    require(scaled == expected, "Cached CDG prescaling must match the original pixels");
    require(frame->scaled(QSize(640, 700), true).cacheKey() == scaled.cacheKey(),
            "Equal fitted sizes must share the same scaled pixels");
    require(frame->scaled(bounds, false) == image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation),
            "Toggling CDG prescaling must use a separate scaling result");
    require(frame->scaled(QSize(300, 400), true) == image,
            "A smaller view must use its own fitted size and skip xBRZ below 2x");

    VideoDisplay view;
    view.resize(bounds);
    view.updateSharedFrame(frame);
    const auto rendered = view.grab().toImage();
    require(rendered.pixelColor(0, 0) == QColor(Qt::black) &&
                rendered.pixelColor(rendered.width() * 3 / 4, rendered.height() / 2) == QColor(Qt::blue),
            "Rendering must preserve centered letterboxing and image colors");
    view.setBackground({});
    view.clearFrame();
    require(!view.hasActiveVideo() && view.grab().toImage() != rendered, "Clearing must release the rendered frame");
}

static void checkStopAndVolume(const QString &filename, MediaBackend::MediaType type)
{
    MediaBackend backend(nullptr, "Stop and volume test", type);
    backend.setAudioOutputDevice("Test Speakers");
    QSlider slider;
    slider.setRange(0, 100);
    // Use the same direct volume binding as the application sliders.
    QObject::connect(&slider, &QSlider::valueChanged, &backend, &MediaBackend::setVolume);
    slider.setValue(60);
    backend.setUseFader(false);
    backend.setMedia(filename);
    QSignalSpy states(&backend, &MediaBackend::stateChanged);
    backend.play();
    waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 3000, "Test audio must start");
    for (const auto &event : states)
        require(event.first().value<MediaBackend::State>() != MediaBackend::StoppedState,
                "Starting a track must not report a cached Stopped state during preroll");
    auto *fader = backend.findChild<AudioFader *>();
    require(fader && fader->state() == AudioFader::FadedIn, "Normal playback must be audible");
    slider.setValue(75);
    require(backend.getVolume() == 75 && fader->state() == AudioFader::FadedIn,
            "Normal volume changes must retain the current fade envelope");

    backend.setUseFader(true);
    backend.stop(false);
    require(fader->state() == AudioFader::FadingOut, "Stop must begin its fade-out");
    slider.setValue(85);
    require(fader->state() == AudioFader::FadingOut, "A slider change must not cancel the stop fade");
    QTimer changes;
    changes.setInterval(30);
    QObject::connect(&changes, &QTimer::timeout, &slider, [&] { slider.setValue((slider.value() + 17) % 101); });
    changes.start();
    waitFor([&] { return backend.state() == MediaBackend::StoppedState; }, 5000,
            "Repeated slider changes must still allow Stop to complete");
    changes.stop();
    slider.setValue(37);
    QTest::qWait(100);
    require(backend.state() == MediaBackend::StoppedState && fader->state() == AudioFader::FadedOut,
            "Changing the volume after Stop must leave playback stopped and silent");
    require(backend.getVolume() == 37, "The requested volume must be retained for the next Play");

    backend.play();
    slider.setValue(63);
    require(fader->state() == AudioFader::FadingIn, "A slider change must also preserve a normal fade-in");
    waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 3000,
            "An explicit Play must still restart playback");
    backend.rawStop();

    if (type == MediaBackend::BackgroundMusic) {
        backend.play(true);
        waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 3000,
                "Silent break music must start");
        slider.setValue(90);
        require(fader->state() == AudioFader::FadedOut,
                "A break-music slider change must preserve automatic ducking");
        backend.rawStop();
    }
}

static void checkSharedVideoFrames(const QString &audioFilename, const QString &cdgFilename)
{
    VideoDisplay singer, preview, rotation;
    for (auto *view : {&singer, &preview, &rotation})
        view->resize(300, 216);
    MediaBackend backend(nullptr, "Shared karaoke video test", MediaBackend::Karaoke);
    backend.setAudioOutputDevice("Test Speakers");
    backend.setUseFader(false);
    backend.setUseSilenceDetection(false);
    backend.setVideoOutputWidgets({&singer, &preview, &rotation});
    QSignalSpy frames(&backend, &MediaBackend::frameReady);
    backend.setMediaCdg(cdgFilename, audioFilename);
    backend.play();
    waitFor([&] {
        return frames.size() >= 3 && singer.hasActiveVideo() && preview.hasActiveVideo() && rotation.hasActiveVideo();
    }, 5000, "All three outputs must receive decoded CDG frames, including hidden views");
    const auto matchingFrames = [&] {
        const auto image = singer.grab().toImage();
        return image == preview.grab().toImage() && image == rotation.grab().toImage();
    };
    const auto frameColor = [&] {
        const auto image = frames.last().at(0).value<QImage>();
        return image.pixelColor(image.width() / 2, image.height() / 2);
    };
    waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 2000, "CDG playback must be running");
    const auto beforeStall = frames.size();
    QElapsedTimer stall;
    stall.start();
    // Sleep without pumping GUI events while the streaming thread crosses a color change.
    while (backend.position() < 900 && stall.elapsed() < 2000)
        QTest::qSleep(10);
    require(backend.position() >= 900 && frames.size() == beforeStall,
            "Playback must advance without delivering frames during a GUI stall");
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    require(frames.size() == beforeStall + 1 && frameColor() == QColor(Qt::green),
            "The first GUI dispatch after a stall must deliver only the latest CDG frame");
    require(matchingFrames(), "All outputs must receive the same frame after a stall");

    backend.pause();
    waitFor([&] { return backend.state() == MediaBackend::PausedState; }, 2000, "CDG playback must pause");
    QTest::qWait(100);
    require(matchingFrames(), "Paused CDG frames must match on all outputs");
    const auto pausedImage = rotation.grab().toImage();
    rotation.show();
    QTest::qWait(40);
    require(rotation.grab().toImage() == pausedImage, "Enabling a hidden view while paused must retain the current frame");

    backend.play();
    waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 2000, "CDG playback must resume");
    QTest::qWait(150); // Allow the coordinated PLAYING request to finish.
    const auto beforeSeek = frames.size();
    const auto priorGeneration = frames.last().at(1).toULongLong();
    QTest::qSleep(100); // Leave a pre-seek frame pending in the mailbox.
    backend.setPosition(2100);
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    require(frames.size() == beforeSeek, "A frame pending before a seek must be discarded");
    waitFor([&] {
        return frames.size() > beforeSeek && backend.position() >= 2000 && frameColor() == QColor(Qt::blue);
    }, 5000, "Playback must resume with frames from the new position after seeking");
    require(frames.last().at(1).toULongLong() != priorGeneration, "A seek must invalidate the previous frame generation");
    require(matchingFrames(), "CDG outputs must still match after a seek");

    QTest::qSleep(100); // Stop with a real decoded frame pending.
    const auto beforeStop = frames.size();
    backend.rawStop();
    QCoreApplication::sendPostedEvents(&backend, QEvent::MetaCall);
    require(frames.size() == beforeStop && !singer.hasActiveVideo() && !preview.hasActiveVideo() &&
                !rotation.hasActiveVideo(), "Stop must clear all outputs and reject pending frames");

    backend.play();
    waitFor([&] { return frames.size() > beforeStop && singer.hasActiveVideo(); }, 5000, "CDG must restart after Stop");
    waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 2000, "Restarted CDG must be playing");
    QTest::qSleep(100);
    const auto beforeTrackChange = frames.size();
    backend.setMedia(audioFilename);
    backend.play();
    waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 3000, "Replacement audio track must start");
    QTest::qWait(100);
    require(frames.size() == beforeTrackChange && !singer.hasActiveVideo() && !preview.hasActiveVideo() &&
                !rotation.hasActiveVideo(), "A track change must discard pending frames from the previous track");

    // Stop before the CDG preroll worker finishes. The state notification can
    // arrive early, but another track must wait for native cleanup.
    backend.setMediaCdg(cdgFilename, audioFilename);
    QSignalSpy stops(&backend, &MediaBackend::stopCompleted);
    QSignalSpy startupStates(&backend, &MediaBackend::stateChanged);
    backend.play();
    backend.rawStop();
    require(stops.isEmpty() && !backend.isPlaybackStopped(), "Stop completion must wait for native CDG cleanup");
    waitFor([&] { return stops.size() == 1 && backend.isPlaybackStopped(); }, 3000,
            "CDG cancellation must emit exactly one completion when the backend is ready");
    for (const auto &event : startupStates)
        require(event.first().value<MediaBackend::State>() != MediaBackend::PlayingState,
                "Cancelling CDG preroll must not deliver a Playing state");

}

static void checkPreviewDialogs(const QString &audioFilename, const QString &cdgFilename, const QString &zipFilename)
{
    const auto looseAudio = QFileInfo(cdgFilename).absoluteDir().filePath(QFileInfo(cdgFilename).completeBaseName() + ".wav");
    require(QFile::copy(audioFilename, looseAudio), "Cannot create a loose CDG pair");
    auto *zip = zip_open(QFile::encodeName(zipFilename).constData(), ZIP_CREATE, nullptr);
    require(zip, "Cannot create preview ZIP");
    auto discard = qScopeGuard([zip] { zip_discard(zip); });
    for (const auto &[name, path] : {std::pair{"song.wav", audioFilename}, std::pair{"song.cdg", cdgFilename}}) {
        auto *source = zip_source_file(zip, QFile::encodeName(path).constData(), 0, 0);
        require(source, "Cannot read preview ZIP fixture");
        if (zip_file_add(zip, name, source, ZIP_FL_ENC_UTF_8) < 0) {
            zip_source_free(source);
            require(false, "Cannot add preview ZIP fixture");
        }
    }
    require(zip_close(zip) == 0, "Cannot save preview ZIP");
    discard.dismiss();

    struct Extraction {
        QSemaphore started;
        QSemaphore resume;
        std::atomic_bool pauseNext{true};
        std::mutex mutex;
        QString directory;
    };
    const auto extraction = std::make_shared<Extraction>();
    // This sink only reads a log message and protects its own state. It must not
    // hold a logger lock while the test closes or opens another preview.
    spdlog::callback_logger_st("logger", [extraction](const spdlog::details::log_msg &entry) {
        const auto text = QString::fromUtf8(entry.payload.data(), entry.payload.size());
        const QString marker = " audio file to: ";
        const auto offset = text.indexOf(marker);
        if (offset < 0)
            return;
        {
            const std::lock_guard lock(extraction->mutex);
            extraction->directory = QFileInfo(text.mid(offset + marker.size())).absolutePath();
        }
        extraction->started.release();
        if (extraction->pauseNext.exchange(false))
            extraction->resume.acquire();
    });
    const auto releaseLogger = qScopeGuard([extraction] {
        extraction->resume.release();
        spdlog::drop("logger");
    });
    const auto extractedDirectory = [extraction] {
        const std::lock_guard lock(extraction->mutex);
        return extraction->directory;
    };
    const auto openPreview = [](const QString &filename) {
        auto *dialog = new DlgVideoPreview(filename);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto *backend = dialog->findChild<MediaBackend *>();
        require(backend && backend->isMuted(), "Preview must start muted");
        dialog->show();
        return QPointer<DlgVideoPreview>(dialog);
    };

    auto cancelled = openPreview(zipFilename);
    require(extraction->started.tryAcquire(1, 3000), "ZIP extraction must run on a worker");
    const auto cancelledDirectory = extractedDirectory();
    require(QFileInfo(cancelledDirectory).isDir(), "ZIP preparation must own a temporary directory");
    cancelled->close();
    waitFor([&] { return cancelled.isNull(); }, 1000, "Close must not wait for an extracting preview");

    auto reopened = openPreview(cdgFilename);
    extraction->resume.release();
    waitFor([&] { return !QFileInfo::exists(cancelledDirectory); }, 3000, "Cancelled extraction must release its files");
    auto *looseBackend = reopened->findChild<MediaBackend *>();
    waitFor([&] { return looseBackend->state() == MediaBackend::PlayingState; }, 3000,
            "A loose CDG pair must play after reopening");
    require(looseBackend->isMuted(), "Playing a loose pair must preserve preview mute");
    QTest::keyClick(reopened, Qt::Key_Escape);
    waitFor([&] { return reopened.isNull(); }, 3000, "Escape must close a playing preview");

    auto broken = openPreview(zipFilename + ".missing");
    broken->setPlaybackTimeLimit(1);
    waitFor([&] { return broken->findChild<QMessageBox *>() != nullptr; }, 3000, "Missing preview must report an error");
    broken->close();
    waitFor([&] { return broken.isNull(); }, 1000, "A preview error must allow an immediate close");

    auto playing = openPreview(zipFilename);
    auto *zipBackend = playing->findChild<MediaBackend *>();
    QSignalSpy frames(zipBackend, &MediaBackend::frameReady);
    waitFor([&] { return zipBackend->state() == MediaBackend::PlayingState && !frames.isEmpty(); }, 3000,
            "A ZIP preview must decode and play");
    const auto playingDirectory = extractedDirectory();
    require(QFileInfo(playingDirectory).isDir() && zipBackend->isMuted(), "Playing ZIP must retain its extracted files and mute");
    QTest::qWait(1100);
    require(playing && playing->isVisible(), "A closed preview's timer must not affect the reopened preview");
    playing->close();
    require(playing && QFileInfo(playingDirectory).isDir(), "Closing a ZIP preview must retain files until stop completion");
    waitFor([&] { return playing.isNull(); }, 3000, "A playing ZIP preview must close after backend cleanup");
    require(!QFileInfo::exists(playingDirectory), "A closed preview must release extracted files");
}

static void checkPreviewAudio(const QString &cdgFilename)
{
    Settings settings;
    auto *storage = settings.findChild<QSettings *>();
    storage->remove("previewAudioOutputDevice");
    storage->remove("previewStartsMuted");
    const auto restore = qScopeGuard([&] {
        storage->remove("previewAudioOutputDevice");
        storage->remove("previewStartsMuted");
        storage->sync();
    });
    require(settings.previewAudioOutputDevice().isEmpty() && settings.previewStartsMuted(),
            "Preview must default to None and Start previews muted");
    settings.setAudioOutputDevice("Default");
    settings.setAudioOutputDeviceBm("Test Speakers");
    require(MediaBackend::getPreviewOutputDevices({}) == QStringList{"Test USB"},
            "The system default and assigned Break Music output must be excluded");
    require(MediaBackend::getPreviewOutputDevices({"Test USB"}).isEmpty(),
            "An output still in use during a pending change must be excluded");
    settings.setAudioOutputDeviceBm("Test USB");
    require(MediaBackend::getPreviewOutputDevices({}).isEmpty(),
            "An independent Break Music output must also be excluded");
    settings.setAudioOutputBmFollowsKaraoke(true);
    require(MediaBackend::getPreviewOutputDevices({}) == QStringList{"Test USB"},
            "Follow Karaoke must reserve the effective Break Music device");

    const auto play = [&](bool usable, bool startsMuted, bool disconnect = false) {
        DlgVideoPreview dialog(cdgFilename);
        auto *backend = dialog.findChild<MediaBackend *>();
        auto *button = dialog.findChild<QToolButton *>("toolButtonMute");
        auto *volume = dialog.findChild<QSlider *>("sliderVolume");
        require(backend && button && button->isEnabled() == usable && backend->isMuted() == startsMuted,
                "The preview controls must reflect the available output and saved mute default");
        require(volume && volume->isEnabled() == usable && volume->value() == 100,
                "The preview volume slider must reflect output availability and start at full volume");
        if (disconnect)
            connected = true;
        dialog.show();
        waitFor([&] { return backend->state() == MediaBackend::PlayingState; }, 3000,
                "Preview video must play even without an audio device");
        if (!usable) {
            button->click();
            require(backend->isMuted(), "A disabled mute button must leave the preview silent");
            return;
        }
        if (startsMuted) {
            waitFor([] { return silentPreviewBuffers > 0; }, 3000, "The preview sink must receive muted audio");
            require(audiblePreviewBuffers == 0, "Start muted must prevent any audible samples at startup");
            button->click();
        }
        waitFor([] { return audiblePreviewBuffers > 0; }, 3000, "Unmuted preview must reach the chosen output");
        const int beforeZero = silentPreviewBuffers;
        volume->setValue(0);
        waitFor([&] { return silentPreviewBuffers > beforeZero; }, 3000, "Zero preview volume must silence the sink");
        require(!backend->isMuted(), "Moving the preview volume slider must not toggle mute");
        const int beforeRestore = audiblePreviewBuffers;
        volume->setValue(100);
        waitFor([&] { return audiblePreviewBuffers > beforeRestore; }, 3000, "Raising preview volume must restore audio");
        const int beforeMute = silentPreviewBuffers;
        button->click();
        waitFor([&] { return silentPreviewBuffers > beforeMute; }, 3000, "Muting must silence the chosen output");
        settings.setPreviewAudioOutputDevice({});
        dialog.validateAudioOutput({});
        require(!button->isEnabled() && !volume->isEnabled() && backend->isMuted(),
                "Changing routes must disable audio in an open preview");
        waitFor([&] { return !dialog.isVisible() && backend->isPlaybackStopped(); }, 3000,
                "The preview must release its audio sink when the route changes");
        settings.setPreviewAudioOutputDevice("Test USB");
    };

    play(false, true);
    settings.setPreviewAudioOutputDevice("Test Speakers");
    settings.setPreviewStartsMuted(false);
    play(false, true);
    settings.setPreviewAudioOutputDevice("Test USB");
    connected = false;
    play(false, true, true);
    settings.setPreviewStartsMuted(true);
    silentPreviewBuffers = audiblePreviewBuffers = 0;
    play(true, true);
    settings.setPreviewStartsMuted(false);
    settings.sync();
    Settings reloaded;
    require(reloaded.previewAudioOutputDevice() == "Test USB" && !reloaded.previewStartsMuted(),
            "The selected device and start-muted preference must survive reopening settings");
    silentPreviewBuffers = audiblePreviewBuffers = 0;
    play(true, false);
}

#ifdef Q_OS_MACOS
static void checkKaraokeMixUsesPreVolumeAudio(const QString &filename, const QString &output)
{
    MediaBackend backend(nullptr, "Recording music tap test", MediaBackend::Karaoke);
    backend.setAudioOutputDevice("Test Speakers");
    backend.setVolume(0);
    backend.setMedia(filename);
    backend.play();
    waitFor([&] { return backend.state() == MediaBackend::PlayingState; }, 3000, "Mix test track must start");

    auto *pad = backend.recordingMusicPad();
    require(pad, "Karaoke must expose a pad before the live volume control");
    const auto releasePad = qScopeGuard([&] { gst_object_unref(pad); });
    auto *mic = gst_element_factory_make("audiotestsrc", nullptr);
    require(mic, "Cannot create the test microphone");
    g_object_set(mic, "is-live", TRUE, "wave", 4, nullptr); // Silence.
    RecordingMix recording;
    QSignalSpy errors(&recording, &RecordingMix::error);
    QSignalSpy finished(&recording, &RecordingMix::finished);
    require(recording.start(mic, pad, output, RecordingMix::Format::Wav, 0, 80), "Mix recording must start");
    QTest::qWait(600);
    backend.rawStop();
    recording.finish();
    waitFor([&] { return finished.size() == 1; }, 4000, "Mix recording must finalize after playback stops");
    require(errors.isEmpty(), "Mix recording must finish without an error");
    QFile wave(output);
    require(wave.open(QIODevice::ReadOnly), "Mix recording must produce a WAV file");
    const auto data = wave.readAll();
    bool hasMusic = false;
    for (qsizetype i = 4096; i + 1 < data.size(); i += 2) {
        if (std::abs(qFromLittleEndian<qint16>(data.constData() + i)) > 1500) {
            hasMusic = true;
            break;
        }
    }
    require(hasMusic, "Muted live playback must still supply backing audio to the recording");
}
#endif

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    gst_init(nullptr, nullptr);
    if (app.arguments().contains("--list-native")) {
        MediaBackend backend(nullptr, "Output probe", MediaBackend::SFX);
        for (const auto &name : backend.getOutputDevices()) {
            std::puts(name.toUtf8().constData());
            std::fflush(stdout);
        }
        return 0;
    }
    // Disable native providers; only the virtual devices should be probed.
    auto *providers = gst_device_provider_factory_list_get_device_providers(GST_RANK_NONE);
    for (auto *entry = providers; entry; entry = entry->next)
        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(entry->data), GST_RANK_NONE);
    gst_plugin_feature_list_free(providers);
    gst_device_provider_register(nullptr, "testaudio", GST_RANK_PRIMARY,
                                 test_audio_provider_get_type());
    try {
        checkSettingsOwnershipAndPersistence();
        checkVideoSettingsReuse();
        checkSharedRendering();
        Settings settings;
        settings.setAudioOutputDevice("Default");
        settings.setAudioOutputDeviceBm("Default");
        settings.setAudioOutputBmFollowsKaraoke(false);
        MediaBackend karaoke(nullptr, "Test karaoke", MediaBackend::Karaoke);
        MediaBackend breaks(nullptr, "Test breaks", MediaBackend::BackgroundMusic);
        require(karaoke.getOutputDevices() == QStringList{"Default", "Test Speakers", "Test USB"},
                "Outputs must include real choices and exclude inputs");
        karaoke.setAudioOutputDevice("Test Speakers");
        breaks.setAudioOutputDevice("Test USB");
        require(created == QStringList{"Test Speakers", "Test USB"},
                "Each backend must create its own selected device sink");
        karaoke.play();
        karaoke.setAudioOutputDevice("Test USB");
        require(created.size() == 2, "Selection must not replace a running sink");
        karaoke.rawStop();
        karaoke.play();
        require(created.size() == 3 && created.last() == "Test USB",
                "Pending selection must apply on restart");
        karaoke.rawStop();
        karaoke.setAudioOutputDevice("Default");
        karaoke.setAudioOutputDevice("Test Speakers");
        require(created.size() == 4, "Returning from Default must allow explicit selection");
        connected = false;
        require(karaoke.getOutputDevices() == QStringList{"Default"},
                "Reopening Settings must refresh disconnected devices");
        karaoke.setAudioOutputDevice("Unavailable output");
        require(created.size() == 4, "Missing outputs must fall back to Default");
        connected = true;
        settings.setAudioOutputDevice("Test USB");
        MediaBackend restored(nullptr, "Test restored", MediaBackend::Karaoke);
        require(created.size() == 5 && created.last() == "Test USB",
                "Startup must apply the saved output choice");
        settings.setAudioOutputDeviceBm("Test Speakers");
        settings.setAudioOutputBmFollowsKaraoke(true);
        Settings savedSettings;
        require(savedSettings.audioOutputBmFollowsKaraoke(), "Follow preference must persist");
        MediaBackend following(nullptr, "Test following", MediaBackend::BackgroundMusic);
        require(created.size() == 6 && created.last() == "Test USB",
                "Break Music must start on the karaoke output when following");
        settings.setAudioOutputDevice("Default");
        require(settings.effectiveAudioOutputDeviceBm() == "Default",
                "Follow mode must reflect changes to the karaoke output");
        following.setAudioOutputDevice(settings.effectiveAudioOutputDeviceBm());
        require(settings.audioOutputDeviceBm() == "Test Speakers",
                "Following must preserve the independent Break Music choice");
        settings.setAudioOutputBmFollowsKaraoke(false);
        following.setAudioOutputDevice(settings.effectiveAudioOutputDeviceBm());
        require(created.size() == 7 && created.last() == "Test Speakers",
                "Disabling follow must restore the independent output");
        QTemporaryDir temporary;
        require(temporary.isValid(), "Cannot create temporary audio directory");
        const auto filename = temporary.filePath("stop-volume.wav");
        {
            QFile file(filename);
            require(file.open(QIODevice::WriteOnly), "Cannot create test audio");
            QDataStream wave(&file);
            wave.setByteOrder(QDataStream::LittleEndian);
            wave.writeRawData("RIFF", 4);
            wave << quint32(480036);
            wave.writeRawData("WAVEfmt ", 8);
            wave << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000)
                 << quint16(2) << quint16(16);
            wave.writeRawData("data", 4);
            wave << quint32(480000);
            for (int sample = 0; sample < 240000; ++sample)
                wave << qint16(12000 * std::sin(2.0 * std::numbers::pi * 400.0 * sample / 8000));
        }
        settings.setAudioOutputDevice("Test Speakers");
        settings.setAudioOutputDeviceBm("Test Speakers");
        checkStopAndVolume(filename, MediaBackend::Karaoke);
        checkStopAndVolume(filename, MediaBackend::BackgroundMusic);
#ifdef Q_OS_MACOS
        checkKaraokeMixUsesPreVolumeAudio(filename, temporary.filePath("muted live playback.wav"));
#endif
        const auto cdgFilename = temporary.filePath("shared-video.cdg");
        {
            QFile cdg(cdgFilename);
            require(cdg.open(QIODevice::WriteOnly), "Cannot create test CDG");
            QByteArray packets(24 * 300 * 30, '\0');
            for (int i = 0; i < 300 * 30; ++i) {
                packets[i * 24] = 9;
                if (i % 30 == 0) {
                    packets[i * 24 + 1] = 30; // Load the low palette.
                    packets[i * 24 + 6] = 0x3c; // Color 1: red.
                    packets[i * 24 + 8] = 0x03; // Color 2: green.
                    packets[i * 24 + 9] = 0x30;
                    packets[i * 24 + 11] = 0x0f; // Color 3: blue.
                } else {
                    packets[i * 24 + 1] = 1; // Memory preset.
                    packets[i * 24 + 4] = i < 150 ? 1 : (i < 450 ? 2 : 3);
                }
            }
            require(cdg.write(packets) == packets.size(), "Cannot write test CDG");
        }
        checkSharedVideoFrames(filename, cdgFilename);
        checkPreviewDialogs(filename, cdgFilename, temporary.filePath("preview.zip"));
        checkPreviewAudio(cdgFilename);
        std::puts("Settings ownership/persistence, live video preferences, audio routing, fades, and preview checks passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
