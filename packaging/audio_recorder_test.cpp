#include "audiorecorder.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QtEndian>
#include <cmath>
#include <numbers>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

class AudioRecorderTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir temporary;
    QList<std::function<void(bool)>> replies;
    std::unique_ptr<Settings> settings;
    QString savedDirectory;
    bool savedEnabled{false};

    AudioRecorder::PermissionRequest mockPermission()
    {
        return [this](QObject *, std::function<void(bool)> reply) {
            replies.append(std::move(reply));
        };
    }

    void answer(qsizetype index, bool granted)
    {
        auto reply = std::move(replies[index]);
        reply(granted);
    }

    static void expectFolderError(const QSignalSpy &errors)
    {
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("Could not create the recording folder"));
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(temporary.isValid());
        settings = std::make_unique<Settings>();
        savedDirectory = settings->recordingOutputDir();
        savedEnabled = settings->recordingEnabled();
        QFile blocker(temporary.filePath("regular-file"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
    }

    void init()
    {
        replies.clear();
        settings->setRecordingEnabled(true);
        // A granted request reaches a deterministic failure before Qt opens capture hardware.
        settings->setRecordingOutputDir(temporary.filePath("regular-file"));
    }

    void cleanupTestCase()
    {
        settings->setRecordingOutputDir(savedDirectory);
        settings->setRecordingEnabled(savedEnabled);
    }

    void filenamesPreserveUnicodeAndExcludePathCharacters()
    {
        const QDateTime time(QDate(2026, 11, 11), QTime(20, 30));
        const auto name = AudioRecorder::recordingFileBaseName(
            QStringLiteral("Zoë 🎤"), QStringLiteral("AC/DC"),
            QStringLiteral("A:B <曲> \"live\" \\ | ?*") + QChar(0) + QChar(127), time);
        QCOMPARE(name, QStringLiteral("Zoë 🎤 - AC_DC - A_B _曲_ _live_ _ _ ____ - 2026-11-11-2030"));

        const auto reserved = AudioRecorder::recordingFileBaseName(" CON.txt ", "LPT²", "..", time);
        QCOMPARE(reserved, QStringLiteral("_CON.txt - _LPT² - Unknown title - 2026-11-11-2030"));
        const auto blank = AudioRecorder::recordingFileBaseName("  ", "", ".", time);
        QCOMPARE(blank, QStringLiteral("Unknown singer - Unknown artist - Unknown title - 2026-11-11-2030"));

        const auto longName = AudioRecorder::recordingFileBaseName(
            QStringLiteral("あ").repeated(100), QStringLiteral("é").repeated(100),
            QStringLiteral("🎶").repeated(100), time) + ".wav";
        QVERIFY(longName.toUtf8().size() < 255);
        QVERIFY(!longName.contains(QChar::ReplacementCharacter));
        QFile output(temporary.filePath(longName));
        QVERIFY2(output.open(QIODevice::WriteOnly), qPrintable(output.errorString()));
    }

    void stopBeforePermissionSuppressesLateGrant()
    {
        AudioRecorder recorder(nullptr, mockPermission());
        QSignalSpy errors(&recorder, &AudioRecorder::recordingError);
        recorder.record("First", "Artist", "Song");
        QCOMPARE(replies.size(), 1);
        recorder.stop();
        answer(0, true);
        QVERIFY(errors.isEmpty());
        QVERIFY(!recorder.findChild<QMediaRecorder *>());
    }

    void songChangeSuppressesOldReplies()
    {
        AudioRecorder recorder(nullptr, mockPermission());
        QSignalSpy errors(&recorder, &AudioRecorder::recordingError);
        recorder.record("First", "Artist", "Old song");
        // The host accepts another song while the outgoing song is still fading.
        recorder.cancelPendingRecording();
        answer(0, true);
        QVERIFY(errors.isEmpty());

        recorder.record("Second", "Artist", "Second song");
        recorder.record("Third", "Artist", "Third song");
        QCOMPARE(replies.size(), 3);
        answer(2, true);
        expectFolderError(errors);
        answer(1, false);
        QCOMPARE(errors.size(), 1);
        QVERIFY(!recorder.findChild<QMediaRecorder *>());
    }

    void pausedGrantWaitsForResumeAndCanBeCancelled()
    {
        AudioRecorder recorder(nullptr, mockPermission());
        QSignalSpy errors(&recorder, &AudioRecorder::recordingError);
        recorder.record("Singer", "Artist", "Song");
        recorder.pause();
        answer(0, true);
        QVERIFY(errors.isEmpty());
        recorder.unpause();
        expectFolderError(errors);
        errors.clear();

        recorder.record("Singer", "Artist", "Song");
        recorder.pause();
        answer(1, true);
        recorder.stop();
        recorder.unpause();
        QVERIFY(errors.isEmpty());
    }

    void currentDenialIsReportedAndDisabledRecordingDoesNotRequestAccess()
    {
        AudioRecorder recorder(nullptr, mockPermission());
        QSignalSpy errors(&recorder, &AudioRecorder::recordingError);
        recorder.record("Singer", "Artist", "Song");
        answer(0, false);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("Microphone permission"));

        recorder.record("Singer", "Artist", "Song");
        settings->setRecordingEnabled(false);
        answer(1, true);
        QCOMPARE(errors.size(), 1);
        recorder.record("Singer", "Artist", "Song");
        QCOMPARE(replies.size(), 2);
    }

    void mixRecordsBothSourcesIndependentlyOfLiveVolume_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("extension");
        QTest::newRow("wav") << int(RecordingMix::Format::Wav) << ".wav";
        QTest::newRow("mp3") << int(RecordingMix::Format::Mp3) << ".mp3";
        QTest::newRow("ogg") << int(RecordingMix::Format::Ogg) << ".ogg";
    }

    void mixRecordsBothSourcesIndependentlyOfLiveVolume()
    {
#ifndef Q_OS_MACOS
        QSKIP("OpenKJ mix is enabled on macOS.");
#endif
        QFETCH(int, format);
        QFETCH(QString, extension);
        gst_init(nullptr, nullptr);
        GError *parseError = nullptr;
        auto *playback = gst_parse_launch("audiotestsrc is-live=true freq=400 samplesperbuffer=480 "
            "! audio/x-raw,format=F32LE,rate=48000,channels=2 ! identity name=tap "
            "! volume volume=0 ! fakesink sync=true", &parseError);
        QVERIFY(!parseError && playback);
        const auto cleanup = qScopeGuard([&] {
            gst_element_set_state(playback, GST_STATE_NULL);
            gst_object_unref(playback);
        });
        auto *element = gst_bin_get_by_name(GST_BIN(playback), "tap");
        auto *pad = gst_element_get_static_pad(element, "src");
        gst_object_unref(element);
        const auto releasePad = qScopeGuard([&] { gst_object_unref(pad); });
        auto *microphone = gst_element_factory_make("audiotestsrc", nullptr);
        QVERIFY(microphone);
        g_object_set(microphone, "is-live", TRUE, "freq", 1000.0, "samplesperbuffer", 480, nullptr);
        QVERIFY(gst_element_set_state(playback, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);

        RecordingMix mix;
        QSignalSpy errors(&mix, &RecordingMix::error);
        QSignalSpy finished(&mix, &RecordingMix::finished);
        const auto path = temporary.filePath("two tones" + extension);
        QVERIFY(mix.start(microphone, pad, path, static_cast<RecordingMix::Format>(format), 25, 75));
        QTest::qWait(650);
        gst_element_set_state(playback, GST_STATE_PAUSED);
        mix.pause();
        QTest::qWait(400);
        gst_element_set_state(playback, GST_STATE_PLAYING);
        mix.resume();
        QTest::qWait(650);
        if (extension == ".ogg") {
            gst_element_set_state(playback, GST_STATE_PAUSED);
            mix.pause();
        }
        mix.finish();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 4000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = file.readAll();
        QVERIFY(data.size() > 1000);
        if (extension == ".ogg") {
            QVERIFY(data.startsWith("OggS"));
            return;
        }
        if (extension == ".mp3")
            return;

        QVERIFY(data.startsWith("RIFF"));
        qsizetype offset = 12;
        while (offset + 8 < data.size() && data.mid(offset, 4) != "data") {
            const auto size = qFromLittleEndian<quint32>(data.constData() + offset + 4);
            offset += 8 + size + (size & 1);
        }
        QVERIFY(offset + 8 < data.size());
        const auto frames = qFromLittleEndian<quint32>(data.constData() + offset + 4) / 4;
        QVERIFY(frames > 48000 && frames < 80000);
        QVERIFY(offset + 8 + 4 * frames <= data.size());
        const auto magnitude = [&](int frequency) {
            double real = 0;
            double imaginary = 0;
            for (int i = 14400; i < 24000; ++i) {
                const double angle = 2.0 * std::numbers::pi * frequency * i / 48000;
                const double sample = qFromLittleEndian<qint16>(data.constData() + offset + 8 + 4 * i);
                real += sample * std::cos(angle);
                imaginary += sample * std::sin(angle);
            }
            return std::hypot(real, imaginary) / 4800;
        };
        const auto mic = magnitude(1000);
        const auto music = magnitude(400);
        QVERIFY(mic > 1000);
        QVERIFY2(music / mic > 2.4 && music / mic < 3.6,
                 qPrintable(QString("Music %1, mic %2").arg(music).arg(mic)));
    }

    void noRecordingFolderIsReportedWithoutCreatingRecorder()
    {
        settings->setRecordingOutputDir(QString());
        AudioRecorder recorder(nullptr, mockPermission());
        QSignalSpy errors(&recorder, &AudioRecorder::recordingError);
        recorder.record("Singer", "Artist", "Song");
        answer(0, true);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains("Choose a recording output folder"));
        QVERIFY(!recorder.findChild<QMediaRecorder *>());
    }
};

QTEST_GUILESS_MAIN(AudioRecorderTest)
#include "audio_recorder_test.moc"
