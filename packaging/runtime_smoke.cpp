// Isolated build-environment check: never opens the host's settings or library.
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QWidget>
#include <gst/gst.h>
#include <zip.h>
#include <cstdio>
#include <cmath>
#include "audionormalization.h"
#include "trackgaincache.h"
#include <QDataStream>
#include <QFile>
#include "localmediauri.h"
#include "mzarchive.h"
#include "zipfilepath.h"
#include "bundledruntime.h"
#include "mediainitialization.h"

static void stage(const char *message)
{
    std::fprintf(stderr, "Runtime check: %s\n", message);
    std::fflush(stderr);
}

static bool runPipeline(const QString &description);

static bool checkZip(const QByteArray &audio, const QByteArray &graphics)
{
    QTemporaryDir directory;
    if (!directory.isValid())
        return false;
    const auto sourceDir = directory.filePath(QStringLiteral("歌手 café"));
    if (!QDir().mkpath(sourceDir))
        return false;
    const auto path = QDir(sourceDir).filePath(QStringLiteral("演奏 🎤.zip"));
    int error = 0;
    auto *archive = zip_open(okj::zipFilePath(path).constData(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    if (!archive)
        return false;
    const auto add = [archive](const char *name, const QByteArray &payload) {
        auto *source = zip_source_buffer(archive, payload.constData(), payload.size(), 0);
        if (!source)
            return false;
        if (zip_file_add(archive, name, source, 0) >= 0)
            return true;
        zip_source_free(source);
        return false;
    };
    if (!add("track.wav", audio) || !add("track.cdg", graphics)) {
        zip_discard(archive);
        return false;
    }
    if (zip_close(archive) < 0) {
        zip_discard(archive);
        return false;
    }
    MzArchive karaoke(path);
    if (!karaoke.isValidKaraokeFile() || karaoke.getSongDuration() <= 0 ||
        !karaoke.extractAudio(directory.path(), "track.wav") ||
        !karaoke.extractCdg(directory.path(), "track.cdg"))
        return false;
    QFile audioFile(directory.filePath("track.wav"));
    QFile cdgFile(directory.filePath("track.cdg"));
    if (!audioFile.open(QIODevice::ReadOnly) || audioFile.readAll() != audio ||
        !cdgFile.open(QIODevice::ReadOnly) || cdgFile.readAll() != graphics)
        return false;
    audioFile.close();
    cdgFile.close();
    const auto uri = localMediaUri(audioFile.fileName());
    return runPipeline("playbin uri=\"" + QString::fromUtf8(uri) +
                       "\" audio-sink=\"fakesink sync=false\" video-sink=fakesink");
}

static bool runPipeline(const QString &description)
{
    GError *error = nullptr;
    auto *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (error || !pipeline) {
        qCritical() << "Pipeline creation failed:" << (error ? error->message : "no pipeline");
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(pipeline);
        return false;
    }
    auto *bus = gst_element_get_bus(pipeline);
    const auto state = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    auto *message = state == GST_STATE_CHANGE_FAILURE ? nullptr : gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        qCritical() << error->message << debug;
        g_free(debug);
        g_clear_error(&error);
    }
    if (!ok)
        qCritical() << "Pipeline did not reach EOS:" << description;
    if (message)
        gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return ok;
}

int main(int argc, char **argv)
{
    stage("initialize Qt GUI");
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        std::fflush(stderr);
    });
    QApplication app(argc, argv);
    spdlog::register_logger(spdlog::default_logger()->clone("logger"));
    QCommandLineParser arguments;
    arguments.addHelpOption();
    arguments.addOption({"media-directory", "Decode every media file in this directory.", "directory"});
    arguments.addOption({"bundled-macos", "Test the packaged macOS plugin set."});
    arguments.process(app);
    stage("show offscreen widget");
    QWidget widget;
    widget.resize(320, 200);
    widget.show();
    app.processEvents();
    stage("SQLite plugin and queries");
    auto db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(":memory:");
    if (!db.open()) {
        qCritical() << "SQLite plugin failed:" << db.lastError();
        return 1;
    }
    QSqlQuery query(db);
    if (!query.exec("CREATE TABLE songs (id INTEGER PRIMARY KEY, duration INTEGER)") ||
        !query.exec("INSERT INTO songs VALUES (1, 180000)") ||
        !query.exec("SELECT duration FROM songs WHERE id=1") || !query.next() ||
        query.value(0).toInt() != 180000) {
        qCritical() << "SQLite query failed:" << query.lastError();
        return 1;
    }

    stage("initialize GStreamer in the background");
    configureWindowsBundledGStreamer();
    QFutureWatcher<MediaInitializationResult> initialization;
    QEventLoop startupLoop;
    QObject::connect(&initialization, &QFutureWatcher<MediaInitializationResult>::finished,
                     &startupLoop, &QEventLoop::quit);
    initialization.setFuture(initializeMediaAsync());
    startupLoop.exec();
    const auto startupResult = initialization.result();
    if (!startupResult.success) {
        qCritical() << "Media initialization failed:" << startupResult.error;
        return 1;
    }
    stage("find media elements");
    for (const auto *name : {"playbin", "pitch", "scaletempo", "equalizer-10bands", "rganalysis",
             "audiopanorama", "volume", "audioconvert", "audioresample", "autoaudiosink", "appsink", "cdgparse",
             "cdgdec", "qtdemux", "avdec_h264",
#ifdef Q_OS_WIN
             "wasapi2sink", "wasapisink", "directsoundsink",
#endif
             }) {
        auto *factory = gst_element_factory_find(name);
        if (!factory) {
            qCritical() << "Missing required GStreamer element:" << name;
            return 1;
        }
        gst_object_unref(factory);
    }

    stage("play a local file URI with spaces, Unicode and reserved characters");
    QTemporaryDir mediaDirectory;
    if (!mediaDirectory.isValid())
        return 1;
    const auto audioPath = mediaDirectory.filePath(QString::fromUtf8("song café #1 100%.wav"));
    QFile audioFile(audioPath);
    if (!audioFile.open(QIODevice::WriteOnly))
        return 1;
    // Generate loud mono PCM without needing an external audio encoder.
    constexpr quint32 sampleRate = 8000;
    constexpr quint32 samples = sampleRate * 3;
    constexpr quint32 dataSize = samples * 2;
    QDataStream wav(&audioFile);
    wav.setByteOrder(QDataStream::LittleEndian);
    wav.writeRawData("RIFF", 4);
    wav << quint32(36 + dataSize);
    wav.writeRawData("WAVEfmt ", 8);
    wav << quint32(16) << quint16(1) << quint16(1) << sampleRate
        << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    wav.writeRawData("data", 4);
    wav << dataSize;
    for (quint32 i = 0; i < samples; ++i)
        wav << qint16(28000 * std::sin(6.283185307 * 440 * i / sampleRate));
    if (wav.status() != QDataStream::Ok || !audioFile.flush())
        return 1;
    audioFile.close();
    const auto uri = localMediaUri(audioPath);
    const auto url = QUrl::fromEncoded(uri);
    if (!url.isLocalFile() || url.toLocalFile() != audioPath ||
        !uri.contains("%20") || !uri.contains("%23") || !uri.contains("%25")) {
        qCritical() << "Incorrect local media URI:" << uri;
        return 1;
    }
#ifdef Q_OS_WIN
    // A drive letter must be in the path, not parsed as a URI authority.
    if (audioPath.size() > 1 && audioPath[1] == ':' && !uri.startsWith("file:///"))
        return 1;
#endif
    if (!runPipeline("playbin uri=\"" + QString::fromUtf8(uri) +
                     "\" audio-sink=\"fakesink sync=false\" video-sink=fakesink"))
        return 1;
    stage("analyze WAV and persist its gain");
    const std::atomic_bool cancelled{false};
    const auto gain = okj::analyzeTrackGain(audioPath, cancelled);
    if (!gain || *gain <= 0.0 || *gain >= 1.0) {
        qCritical() << "Expected loud WAV to produce attenuation";
        return 1;
    }
    const auto libraryPath = mediaDirectory.filePath("library.sqlite");
    const auto key = okj::TrackGainKey::fromFile(audioPath);
    okj::cacheTrackGain(libraryPath, key, *gain);
    if (okj::cachedTrackGain(libraryPath, key) != gain) {
        qCritical() << "Could not read back the measured gain";
        return 1;
    }

    stage("process SoundTouch and EQ audio");
    if (!arguments.isSet("bundled-macos") && !runPipeline("audiotestsrc num-buffers=12 ! audioconvert ! pitch pitch=1.1 ! "
                     "scaletempo ! equalizer-10bands band0=3 ! audiopanorama ! volume ! fakesink"))
        return 1;

    stage("negotiate DSP output with a fixed-format audio device");
    // Unlike fakesink alone, these caps require real format/rate conversion
    // after SoundTouch/EQ, as a native output device may require on Windows.
    const QString fixedOutput =
        "pitch pitch=1.1 ! scaletempo ! equalizer-10bands band0=3 ! "
        "audiopanorama ! volume ! audioconvert ! audioresample ! "
        "audio/x-raw,format=S16LE,rate=48000,channels=2 ! fakesink sync=false";
    if (!arguments.isSet("bundled-macos") && !runPipeline("audiotestsrc num-buffers=12 ! "
                     "audio/x-raw,format=F32LE,rate=44100,channels=2 ! " + fixedOutput))
        return 1;
    // Exercise playbin's negotiation into the same sink chain with the 8 kHz
    // mono WAV above, in addition to the stereo synthetic source.
    if (!runPipeline("playbin uri=\"" + QString::fromUtf8(uri) +
                     "\" audio-sink=\"" + fixedOutput + "\" video-sink=fakesink"))
        return 1;

    stage("decode CDG");
    QTemporaryFile cdg;
    if (!cdg.open())
        return 1;
    QByteArray packets(24 * 600, '\0');
    for (int i = 0; i < 600; ++i) {
        packets[i * 24] = 9; // CDG command
        packets[i * 24 + 1] = 1; // Memory preset
    }
    if (cdg.write(packets) != packets.size() || !cdg.flush())
        return 1;
    QString path = cdg.fileName();
    cdg.close(); // Release the writer before GStreamer opens the file on Windows.
    path.replace('\\', "\\\\");
    path.replace('"', "\\\"");
    if (!runPipeline("filesrc location=\"" + path +
                     "\" ! cdgparse ! cdgdec ! videoconvert ! fakesink sync=false"))
        return 1;

    stage("ZIP round trip for the decoded WAV and CDG fixtures");
    if (!audioFile.open(QIODevice::ReadOnly) || !checkZip(audioFile.readAll(), packets)) {
        qCritical() << "Karaoke ZIP creation/readback failed";
        return 1;
    }

    if (arguments.isSet("media-directory")) {
        stage("decode packaged-format fixtures");
        const QDir directory(arguments.value("media-directory"));
        const auto files = directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
        if (!directory.exists() || files.isEmpty()) {
            qCritical() << "Media fixture directory is missing or empty:" << directory.absolutePath();
            return 1;
        }
        for (const auto &file : files) {
            qInfo() << "Decode fixture:" << file.fileName();
            if (!runPipeline("playbin uri=\"" + QString::fromUtf8(localMediaUri(file.absoluteFilePath())) +
                             "\" audio-sink=\"fakesink sync=false\" video-sink=\"fakesink sync=false\""))
                return 1;
        }
    }
    qInfo() << "PASS: Qt GUI, ZIP round trip, SQLite, local-file URI playback, required media plugins, SoundTouch/EQ audio, output format negotiation, and CDG decoding";
    return 0;
}
