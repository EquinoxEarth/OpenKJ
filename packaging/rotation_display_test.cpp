#include "rotationdisplay.h"
#include "models/tablemodelrotation.h"
#include "videodisplay.h"

#include <QApplication>
#include <QSignalSpy>
#include <QScreen>
#include <QPainter>
#include <QStandardPaths>
#include <QTest>
#include <spdlog/sinks/null_sink.h>
#include <algorithm>

static QList<okj::RotationSinger> singers()
{
    QList<okj::RotationSinger> result;
    const QStringList names{"Alex", "Jamie", "Morgan", "Taylor", "Chris", "Drew"};
    const QList<int> ids{71, 8, 105, 42, 91, 16};
    for (int i = 0; i < names.size(); ++i) {
        okj::RotationSinger singer(ids[i], names[i], i, false, QDateTime::currentDateTime());
        if (i != 1 && i != 3)
            singer.cachedNextSongPath = QString("/test/song-%1.zip").arg(i);
        result.append(singer);
    }
    return result;
}

class RotationDisplayTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        Settings settings;
        settings.setRotationDisplayVisible(false);
        settings.setRotationDisplayFullscreen(false);
        settings.setRotationDisplaySingerCount(5);
        settings.setRotationDisplayShowEmptySingers(false);
        settings.setRotationDisplayHideNameNotes(false);
        settings.setRotationDisplayVideoEnabled(false);
        settings.setRotationDisplayVideoOnLeft(true);
        settings.setRotationDisplayShowHeading(true);
        settings.setRotationDisplayBackgroundColor(QColor("#101820"));
        settings.setRotationDisplayTextColor(QColor("#f1f5f9"));
        QFont font = QApplication::font();
        font.setPointSize(48);
        settings.setRotationDisplayFont(font);
    }

    void orderWrapsOnceAndLimitFollowsFiltering()
    {
        TableModelRotation model;
        QVERIFY(model.upcomingSingerNames(5, false).isEmpty());
        model.onRotationLoaded(singers());
        QCOMPARE(model.upcomingSingerNames(3, false), QStringList({"Alex", "Morgan", "Chris"}));
        QCOMPARE(model.upcomingSingerNames(3, true), QStringList({"Alex", "Jamie", "Morgan"}));
        QVERIFY(model.upcomingSingerNames(0, true).isEmpty());
        model.setCurrentSinger(105);
        QCOMPARE(model.upcomingSingerNames(3, false), QStringList({"Chris", "Drew", "Alex"}));
        QCOMPARE(model.upcomingSingerNames(20, true), QStringList({"Taylor", "Chris", "Drew", "Alex", "Jamie", "Morgan"}));
        QCOMPARE(model.upcomingSingerNames(20, false), QStringList({"Chris", "Drew", "Alex", "Morgan"}));
        auto records = singers();
        records.removeAt(2); // Current singer was removed: begin at the remaining top.
        model.onRotationLoaded(records);
        QCOMPARE(model.upcomingSingerNames(2, false), QStringList({"Alex", "Chris"}));
        model.onRotationLoaded({singers().first()});
        model.setCurrentSinger(71);
        QCOMPARE(model.upcomingSingerNames(5, false), QStringList({"Alex"}));
    }

    void liveChangesAndSettingsUpdateTheWindow()
    {
        TableModelRotation model;
        model.onRotationLoaded(singers());
        RotationDisplay display(model);
        display.setDisplayVisible(true);
        QCOMPARE(display.singerNames(), QStringList({"Alex", "Morgan", "Chris", "Drew"}));
        model.setCurrentSinger(105);
        QTRY_COMPARE(display.singerNames(), QStringList({"Chris", "Drew", "Alex", "Morgan"}));
        Settings settings;
        settings.setRotationDisplaySingerCount(3);
        display.applySettings();
        QCOMPARE(display.singerNames(), QStringList({"Chris", "Drew", "Alex"}));
        settings.setRotationDisplayShowEmptySingers(true);
        display.applySettings();
        QCOMPARE(display.singerNames(), QStringList({"Taylor", "Chris", "Drew"}));
        auto records = singers();
        records[3].name = "Taylor & friends";
        model.onRotationLoaded(records);
        QTRY_COMPARE(display.singerNames().first(), QString("Taylor & friends"));
        settings.setRotationDisplayShowEmptySingers(false);
        records[4].cachedNextSongPath.clear(); // Last unplayed song was consumed/removed.
        model.onRotationLoaded(records);
        QTRY_COMPARE(display.singerNames(), QStringList({"Drew", "Alex", "Morgan"}));
        settings.setRotationDisplayShowEmptySingers(true);
        settings.setRotationDisplaySingerCount(5);
        display.applySettings();
        display.resize(640, 720);
        QTest::qWait(10);
        if (const auto path = qEnvironmentVariable("OPENKJ_ROTATION_PREVIEW"); !path.isEmpty())
            QVERIFY(display.grab().save(path));
        // Exercise short/wide windows, large fonts, long names, and maximum rows.
        records[0].name = QString(200, QLatin1Char('W'));
        model.onRotationLoaded(records);
        settings.setRotationDisplaySingerCount(20);
        settings.setRotationDisplayShowHeading(false);
        display.applySettings();
        for (const auto size : {QSize(320, 240), QSize(1200, 360), QSize(600, 1000)}) {
            display.resize(size);
            QVERIFY(!display.grab().isNull());
        }
        model.onRotationLoaded({});
        QTRY_VERIFY(display.singerNames().isEmpty());
        QCOMPARE(display.accessibleDescription(), QString("Waiting for singers"));
    }

    void nameNotesOnlyAffectAudienceNames()
    {
        auto records = singers();
        records[0].name = "Austin (Wendy)";
        records[2].name = "Austin (Sue)";
        records[4].name = QString::fromUtf8("Zoë (family (Wendy)) Smith (regular)");
        records[5].name = QString::fromUtf8("René (family");
        TableModelRotation model;
        model.onRotationLoaded(records);
        model.setCurrentSinger(71);
        const auto originalNames = model.upcomingSingerNames(5, false);
        RotationDisplay display(model);
        display.setCurrentSinger(records[0].name, true);
        QCOMPARE(display.currentSingerName(), records[0].name);
        QCOMPARE(display.singerNames(), originalNames);
        Settings settings;
        settings.setRotationDisplayHideNameNotes(true);
        display.applySettings();
        QVERIFY(Settings().rotationDisplayHideNameNotes());
        QCOMPARE(display.currentSingerName(), QString("Austin"));
        // Separate singers remain separate even when their displayed names match.
        QCOMPARE(display.singerNames(), QStringList({"Austin", QString::fromUtf8("Zoë Smith"),
                                                     QString::fromUtf8("René"), "Austin"}));
        QVERIFY(!display.accessibleDescription().contains("Wendy"));
        QVERIFY(!display.accessibleDescription().contains("Sue"));
        QCOMPARE(model.upcomingSingerNames(5, false), originalNames);
        QCOMPARE(model.getSinger(71).name, records[0].name);
        display.setCurrentSinger("(Wendy)", true);
        QVERIFY(display.currentSingerName().isEmpty());
        QVERIFY(!display.accessibleDescription().contains("Wendy"));
        display.setCurrentSinger(records[0].name, true);
        settings.setRotationDisplayHideNameNotes(false);
        display.applySettings();
        // Toggling off restores both areas immediately, including the active singer.
        QCOMPARE(display.currentSingerName(), records[0].name);
        QCOMPARE(display.singerNames(), originalNames);
        display.setCurrentSinger(records[0].name, false);
        QVERIFY(display.currentSingerName().isEmpty());
    }

    void videoPanelCanSwapSidesAndRetainPausedFrames()
    {
        TableModelRotation model;
        model.onRotationLoaded(singers());
        RotationDisplay display(model);
        display.resize(1100, 650);
        display.setDisplayVisible(true);
        auto *video = display.videoDisplay();
        QVERIFY(video->isHidden());
        Settings settings;
        settings.setRotationDisplayVideoEnabled(true);
        display.applySettings();
        QTRY_VERIFY(video->isVisible());
        QVERIFY(video->geometry().center().x() < display.width() / 2);
        QVERIFY(display.rect().contains(video->geometry()));
        QVERIFY(video->testAttribute(Qt::WA_TransparentForMouseEvents));
        QImage frame(300, 216, QImage::Format_RGB888);
        frame.fill(Qt::green);
        video->updateFrame(frame);
        const auto frameColour = [video] {
            const auto image = video->grab().toImage();
            return image.pixelColor(image.width() / 2, image.height() / 2);
        };
        QCOMPARE(frameColour(), QColor(Qt::green));
        const auto names = display.singerNames();
        settings.setRotationDisplayVideoOnLeft(false);
        display.applySettings();
        QVERIFY(video->geometry().center().x() > display.width() / 2);
        QCOMPARE(display.singerNames(), names);
        QCOMPARE(frameColour(), QColor(Qt::green));
        settings.setRotationDisplayVideoEnabled(false);
        display.applySettings();
        QVERIFY(video->isHidden());
        // A registered hidden view still retains frames, including a paused image.
        frame.fill(Qt::red);
        video->updateFrame(frame);
        settings.setRotationDisplayVideoEnabled(true);
        display.applySettings();
        QCOMPARE(frameColour(), QColor(Qt::red));
        QVERIFY(Settings().rotationDisplayVideoEnabled());
        QVERIFY(!Settings().rotationDisplayVideoOnLeft());
        RotationDisplay restored(model);
        restored.setDisplayVisible(true);
        QVERIFY(restored.videoDisplay()->geometry().center().x() > restored.width() / 2);
        restored.close();
        if (const auto path = qEnvironmentVariable("OPENKJ_ROTATION_VIDEO_PREVIEW"); !path.isEmpty()) {
            QImage preview(600, 432, QImage::Format_RGB888);
            preview.fill(QColor("#131c2c"));
            QPainter painter(&preview);
            painter.setPen(QColor("#57d9b2"));
            painter.setFont(QFont("Arial", 30, QFont::Bold));
            painter.drawText(preview.rect().adjusted(0, 50, 0, -180), Qt::AlignCenter, "CDG PREVIEW");
            painter.setPen(Qt::white);
            painter.setFont(QFont("Arial", 25));
            painter.drawText(preview.rect().adjusted(0, 180, 0, -50), Qt::AlignCenter, "Current lyric line");
            painter.end();
            video->updateFrame(preview);
            QVERIFY(display.grab().save(path));
        }
        video->clearFrame();
        QVERIFY(!video->hasActiveVideo());
        for (const auto size : {QSize(320, 240), QSize(1200, 360), QSize(600, 1000)}) {
            display.resize(size);
            QVERIFY(display.rect().contains(video->geometry()));
            QVERIFY(!display.grab().isNull());
        }
    }

    void closeFullscreenAndRestartPreserveIndependentState()
    {
        TableModelRotation model;
        model.onRotationLoaded(singers());
        Settings settings;
        RotationDisplay display(model);
        QSignalSpy visibility(&display, &RotationDisplay::visibilityChanged);
        display.setDisplayVisible(true);
        QTRY_VERIFY(display.isVisible());
        display.resize(520, 440);
        const auto windowedSize = display.size();
        display.showFullScreen(); // Also cover fullscreen initiated outside our own controls.
        QTRY_VERIFY(display.isFullScreen());
        QVERIFY(settings.rotationDisplayFullscreen());
        display.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&display));
        QTest::keyClick(&display, Qt::Key_Escape);
        QTRY_VERIFY(!display.isFullScreen());
        QCOMPARE(display.size(), windowedSize);
        QVERIFY(!settings.rotationDisplayFullscreen());
        display.close();
        QTRY_VERIFY(!display.isVisible());
        QVERIFY(!settings.rotationDisplayVisible());
        QVERIFY(visibility.size() >= 2);
        display.move(100000, 100000); // Simulate geometry saved on a missing display.
        display.setDisplayVisible(true);
        const auto screens = QGuiApplication::screens();
        QVERIFY(std::any_of(screens.cbegin(), screens.cend(),
                            [&display](QScreen *screen) {
            return screen->availableGeometry().intersects(display.frameGeometry());
        }));
        display.setFullscreen(true);
        display.saveForShutdown();
        QVERIFY(!display.isVisible());
        QVERIFY(settings.rotationDisplayVisible());
        QVERIFY(settings.rotationDisplayFullscreen());
        RotationDisplay reopened(model);
        reopened.setDisplayVisible(settings.rotationDisplayVisible());
        QTRY_VERIFY(reopened.isFullScreen());
        reopened.setFullscreen(false);
        QCOMPARE(reopened.size(), windowedSize);
        reopened.close();
        settings.setRotationDisplaySingerCount(500);
        QCOMPARE(Settings().rotationDisplaySingerCount(), 20);
        settings.setRotationDisplaySingerCount(-1);
        QCOMPARE(Settings().rotationDisplaySingerCount(), 1);
        settings.setRotationDisplayVisible(true);
        settings.setSafeStartupMode(true);
        QVERIFY(!settings.rotationDisplayVisible());
        settings.setRotationDisplayVisible(false);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    spdlog::null_logger_mt("logger");
    RotationDisplayTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "rotation_display_test.moc"
