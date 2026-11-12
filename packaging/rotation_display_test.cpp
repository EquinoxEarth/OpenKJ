#include "rotationdisplay.h"
#include "models/tablemodelrotation.h"
#include "videodisplay.h"

#include <QApplication>
#include <QDialog>
#include <QHeaderView>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QTreeView>
#include <QSignalSpy>
#include <QScreen>
#include <QScopeGuard>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryDir>
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
        settings.setRotationAltSortOrder(false);
        settings.setRotationDisplayVisible(false);
        settings.setRotationDisplayFullscreen(false);
        settings.setRotationDisplaySingerCount(5);
        settings.setRotationDisplayShowEmptySingers(false);
        settings.setRotationDisplayHideNameNotes(false);
        settings.setRotationDisplayVideoEnabled(false);
        settings.setRotationDisplayVideoOnLeft(true);
        settings.setRotationDisplayShowHeading(true);
        settings.setRotationDisplayBackgroundColor(QColor("#101820"));
        settings.setRotationDisplayBackgroundImage({});
        settings.setRotationDisplayTextColor(QColor("#f1f5f9"));
        QFont font = QApplication::font();
        font.setPointSize(48);
        settings.setRotationDisplayFont(font);
    }

    void cleanup()
    {
        Settings::setSafeStartupMode(false);
        Settings::setSkipLegacyUiRestoreOnUpgrade(false);
    }

    void recoverySkipsSavedUiStateAcrossSettingsInstances()
    {
        QWidget mainWindow;
        QDialog dialog;
        QTableView table;
        QTreeView tree;
        QSplitter splitter;
        QStandardItemModel columns(0, 2);
        mainWindow.setObjectName("recoveryTestMainWindow");
        dialog.setObjectName("recoveryTestDialog");
        table.setObjectName("recoveryTestTable");
        tree.setObjectName("recoveryTestTree");
        splitter.setObjectName("recoveryTestSplitter");
        table.setModel(&columns);
        tree.setModel(&columns);
        mainWindow.resize(480, 360);
        dialog.resize(400, 300);
        table.setColumnWidth(0, 180);
        table.hideColumn(1);
        tree.setColumnWidth(0, 190);
        splitter.setOrientation(Qt::Vertical);

        Settings existingConsumer;
        const auto restoreStartupSettings = qScopeGuard([
            &existingConsumer, startupOk = existingConsumer.lastStartupOk(),
            showCdg = existingConsumer.showCdgWindow(),
            cdgFullscreen = existingConsumer.cdgWindowFullscreen()
        ] {
            Settings::setSafeStartupMode(false);
            existingConsumer.setStartupOk(startupOk);
            existingConsumer.setShowCdgWindow(showCdg);
            existingConsumer.setCdgWindowFullscreen(cdgFullscreen);
        });
        {
            Settings previousRun;
            previousRun.setStartupOk(false);
            previousRun.saveWindowState(&mainWindow);
            previousRun.saveWindowState(&dialog);
            previousRun.saveColumnWidths(&table);
            previousRun.saveColumnWidths(&tree);
            previousRun.saveSplitterState(&splitter);
            previousRun.setShowCdgWindow(true);
            previousRun.setCdgWindowFullscreen(true);
            previousRun.setRotationDisplayVisible(true);
            previousRun.setRotationDisplayFullscreen(true);
            previousRun.setRotationDisplayGeometry(mainWindow.saveGeometry());
        }

        QVERIFY(!existingConsumer.lastStartupOk());
        Settings::setSafeStartupMode(true); // Equivalent to accepting the startup prompt.
        Settings newConsumer;
        QVERIFY(Settings::safeStartupMode());
        QVERIFY(!newConsumer.showCdgWindow());
        QVERIFY(!existingConsumer.cdgWindowFullscreen());
        QVERIFY(!newConsumer.rotationDisplayVisible());
        QVERIFY(!newConsumer.rotationDisplayFullscreen());

        mainWindow.resize(320, 240);
        dialog.resize(300, 220);
        table.setColumnWidth(0, 100);
        table.showColumn(1);
        tree.setColumnWidth(0, 100);
        splitter.setOrientation(Qt::Horizontal);
        existingConsumer.restoreWindowState(&mainWindow);
        newConsumer.restoreWindowState(&dialog);
        QVERIFY(!newConsumer.restoreColumnWidths(&table));
        existingConsumer.restoreColumnWidths(&tree);
        newConsumer.restoreSplitterState(&splitter);
        QCOMPARE(mainWindow.size(), QSize(320, 240));
        QCOMPARE(dialog.size(), QSize(300, 220));
        QCOMPARE(table.columnWidth(0), 100);
        QVERIFY(!table.isColumnHidden(1));
        QCOMPARE(tree.columnWidth(0), 100);
        QCOMPARE(splitter.orientation(), Qt::Horizontal);

        TableModelRotation model;
        RotationDisplay recovered(model);
        recovered.setDisplayVisible(newConsumer.rotationDisplayVisible());
        QVERIFY(!recovered.isVisible());
        QCOMPARE(recovered.size(), QSize(600, 720));
        recovered.saveForShutdown();

        Settings::setSafeStartupMode(false);
        QVERIFY(existingConsumer.showCdgWindow());
        QVERIFY(newConsumer.cdgWindowFullscreen());
        QVERIFY(newConsumer.rotationDisplayVisible());
        QVERIFY(newConsumer.rotationDisplayFullscreen());
        existingConsumer.restoreWindowState(&mainWindow);
        newConsumer.restoreWindowState(&dialog);
        QVERIFY(newConsumer.restoreColumnWidths(&table));
        existingConsumer.restoreColumnWidths(&tree);
        newConsumer.restoreSplitterState(&splitter);
        QCOMPARE(mainWindow.size(), QSize(480, 360));
        QCOMPARE(dialog.size(), QSize(400, 300));
        QCOMPARE(table.columnWidth(0), 180);
        QVERIFY(table.isColumnHidden(1));
        QCOMPARE(tree.columnWidth(0), 190);
        QCOMPARE(splitter.orientation(), Qt::Vertical);

        RotationDisplay normal(model);
        QCOMPARE(normal.size(), QSize(480, 360));
        normal.setDisplayVisible(newConsumer.rotationDisplayVisible());
        QTRY_VERIFY(normal.isFullScreen());
        normal.close();

        Settings::setSafeStartupMode(true);
        TableModelRotation recoveryModel;
        RotationDisplay manuallyOpened(recoveryModel);
        manuallyOpened.setDisplayVisible(true);
        QTRY_VERIFY(manuallyOpened.isVisible());
        QVERIFY(!manuallyOpened.isFullScreen());
        manuallyOpened.close();
    }

    void upgradeRestoresRotationDisplayWithLegacyRestoreDisabled()
    {
        Settings settings;
        TableModelRotation model;
        {
            RotationDisplay previous(model);
            previous.resize(515, 421);
            previous.setDisplayVisible(true);
            previous.setFullscreen(true);
            previous.saveForShutdown();
        }

        Settings::setSkipLegacyUiRestoreOnUpgrade(true);
        QWidget legacyWindow;
        legacyWindow.setObjectName("upgradeTestLegacyWindow");
        legacyWindow.resize(480, 360);
        settings.saveWindowState(&legacyWindow);
        legacyWindow.resize(320, 240);
        settings.restoreWindowState(&legacyWindow);
        QCOMPARE(legacyWindow.size(), QSize(320, 240));

        RotationDisplay upgraded(model);
        QCOMPARE(upgraded.size(), QSize(515, 421));
        upgraded.setDisplayVisible(settings.rotationDisplayVisible());
        QTRY_VERIFY(upgraded.isVisible() && upgraded.isFullScreen());
        upgraded.saveForShutdown();
        QVERIFY(settings.rotationDisplayVisible());
        QVERIFY(settings.rotationDisplayFullscreen());
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

    void playbackOrderMatchesDisplayAcrossSongCompletion_data()
    {
        QTest::addColumn<bool>("keepCurrentOnTop");
        QTest::addColumn<int>("currentRow");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("normal-middle") << false << 1 << QStringList({"C", "D", "A", "B"});
        QTest::newRow("normal-wrap") << false << 3 << QStringList({"A", "B", "C", "D"});
        QTest::newRow("pinned-middle") << true << 1 << QStringList({"A", "C", "D", "B"});
        QTest::newRow("pinned-last") << true << 3 << QStringList({"A", "B", "C", "D"});
        QTest::newRow("pinned-first") << true << 0 << QStringList({"B", "C", "D", "A"});
    }

    void playbackOrderMatchesDisplayAcrossSongCompletion()
    {
        QFETCH(bool, keepCurrentOnTop);
        QFETCH(int, currentRow);
        QFETCH(QStringList, expected);
        Settings settings;
        settings.setRotationAltSortOrder(keepCurrentOnTop);
        QList<okj::RotationSinger> records;
        for (int i = 0; i < 4; ++i) {
            okj::RotationSinger singer(10 + i, QString(QChar('A' + i)), i, false, QDateTime::currentDateTime());
            singer.cachedNextSongPath = QString("/test/%1.mp4").arg(i);
            singer.cachedNextSongArtist = "Artist";
            singer.cachedNextSongTitle = singer.name + " song";
            records.append(singer);
        }
        TableModelRotation model;
        model.onRotationLoaded(records);
        model.setCurrentSinger(records[currentRow].id);
        RotationDisplay display(model);
        QCOMPARE(display.singerNames(), expected);
        // Moving the active singer to the top must not change the announced order.
        if (keepCurrentOnTop)
            model.singerMove(currentRow, 0);
        QCOMPARE(model.upcomingSingerNames(20, false), expected);
        QCOMPARE(model.nextSinger().name, expected.first());
        QTRY_COMPARE(display.singerNames(), expected);
        const auto announcedSong = model.nextSinger().nextSongPath();
        // The stop handler moves the completed singer to the bottom, then clears
        // the active singer before choosing the countdown and playback target.
        if (keepCurrentOnTop) {
            model.singerMove(model.getSinger(model.currentSinger()).position, 3);
            model.setCurrentSinger(-1);
        }
        QCOMPARE(model.upcomingSingerNames(20, false), expected);
        QCOMPARE(model.nextSinger().name, expected.first());
        QCOMPARE(model.nextSinger().nextSongPath(), announcedSong);
        QTRY_COMPARE(display.singerNames(), expected);
        // An edit during the countdown must affect the display and playback's
        // shared choice together, while preserving stable singer/song identity.
        model.singerDelete(model.nextSinger().id);
        expected.removeFirst();
        QCOMPARE(model.nextSinger().name, expected.first());
        QTRY_COMPARE(display.singerNames(), expected);
    }

    void emptyQueuesAndReordersUseTheSameFilteredOrder()
    {
        Settings settings;
        settings.setRotationAltSortOrder(true);
        auto records = singers();
        TableModelRotation model;
        QVERIFY(!model.nextSinger().isValid());
        model.onRotationLoaded(records);
        model.setCurrentSinger(105); // Morgan, originally in the middle.
        model.singerMove(2, 0);
        RotationDisplay display(model);
        QCOMPARE(model.nextSinger().name, QString("Alex"));
        QCOMPARE(display.singerNames(), QStringList({"Alex", "Chris", "Drew", "Morgan"}));
        // Showing empty singers is an audience option; playback skips them.
        settings.setRotationDisplayShowEmptySingers(true);
        display.applySettings();
        QCOMPARE(display.singerNames(), QStringList({"Alex", "Jamie", "Taylor", "Chris", "Drew"}));
        QCOMPARE(model.nextSinger().name, QString("Alex"));
        model.singerMove(model.getSinger(16).position, 1);
        QCOMPARE(model.nextSinger().id, 16); // A host reorder takes effect immediately.
        settings.setRotationDisplayShowEmptySingers(false);
        display.applySettings();
        QCOMPARE(display.singerNames().first(), model.nextSinger().name);
        // No other singer has a song: the current singer can take another turn.
        records = {model.getSinger(105)};
        records.first().position = 0;
        model.onRotationLoaded(records);
        QCOMPARE(model.nextSinger().id, 105);
        QTRY_COMPARE(display.singerNames(), QStringList({"Morgan"}));
        records.first().cachedNextSongPath.clear();
        model.onRotationLoaded(records);
        QVERIFY(!model.nextSinger().isValid());
        QTRY_VERIFY(display.singerNames().isEmpty());
        // Removing the active singer starts at the remaining top.
        model.onRotationLoaded({singers().first()});
        QCOMPARE(model.nextSinger().id, 71);
        model.onRotationLoaded({});
        QVERIFY(!model.nextSinger().isValid());
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

    void accentColourUpdatesCurrentSingerAndFirstNumber()
    {
        Settings settings;
        auto *storage = settings.findChild<QSettings *>();
        const QString key = "rotationDisplay/accentColor";
        const auto saved = storage->value(key);
        const auto restore = qScopeGuard([&] {
            if (saved.isValid())
                storage->setValue(key, saved);
            else
                storage->remove(key);
            settings.sync();
        });
        storage->remove(key);
        const QColor original("#57d9b2");
        const QColor custom("#e87839");
        QCOMPARE(settings.rotationDisplayAccentColor(), original);

        TableModelRotation model;
        model.onRotationLoaded(singers());
        RotationDisplay display(model);
        display.resize(640, 720);
        display.setDisplayVisible(true);
        const auto countColour = [](const QImage &image, QColor colour) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    count += image.pixel(x, y) == colour.rgb();
            return count;
        };
        QVERIFY(countColour(display.grab().toImage(), original) > 0);
        display.setCurrentSinger("Alex", true);
        const auto before = display.grab().toImage();

        settings.setRotationDisplayAccentColor(custom);
        settings.sync();
        QCOMPARE(Settings().rotationDisplayAccentColor(), custom);
        display.applySettings();
        const auto after = display.grab().toImage();
        QCOMPARE(countColour(after, original), 0);
        QCOMPARE(countColour(after, settings.rotationDisplayTextColor()),
                 countColour(before, settings.rotationDisplayTextColor()));
        const int withCurrentSinger = countColour(after, custom);
        display.setCurrentSinger("Alex", false);
        const int firstNumberOnly = countColour(display.grab().toImage(), custom);
        QVERIFY(firstNumberOnly > 0);
        QVERIFY(withCurrentSinger > firstNumberOnly);
    }

    void backgroundImageFillsWindowAndShowsAroundFrames()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("rotation background.png");
        QImage source(300, 100, QImage::Format_RGB32);
        source.fill(Qt::green);
        QPainter painter(&source);
        painter.fillRect(QRect(0, 0, 100, 100), Qt::red);
        painter.fillRect(QRect(200, 0, 100, 100), Qt::blue);
        painter.end();
        QVERIFY(source.save(path));

        Settings settings;
        TableModelRotation model;
        RotationDisplay display(model);
        display.setDisplayVisible(true);
        display.resize(360, 600);
        const auto colourAt = [&display](QPoint point) {
            const auto pixmap = display.grab();
            return pixmap.toImage().pixelColor(point * pixmap.devicePixelRatio());
        };
        QCOMPARE(colourAt({0, 0}), settings.rotationDisplayBackgroundColor());

        settings.setRotationDisplayBackgroundImage(path);
        settings.sync();
        QCOMPARE(Settings().rotationDisplayBackgroundImage(), path);
        display.applySettings();
        QCOMPARE(colourAt({0, 0}), QColor(Qt::green));
        QCOMPARE(colourAt(display.rect().bottomRight()), QColor(Qt::green));

        display.resize(900, 300);
        QCOMPARE(colourAt({0, 0}), QColor(Qt::red));
        QCOMPARE(colourAt(display.rect().bottomRight()), QColor(Qt::blue));
        settings.setRotationDisplayVideoEnabled(true);
        display.applySettings();
        auto *video = display.videoDisplay();
        const QPoint videoSample(video->x() + video->width() / 4, video->geometry().center().y());
        const QPoint paddingSample(video->x() + 2, videoSample.y());
        const QPoint frameCenter = video->geometry().center();
        QCOMPARE(colourAt(videoSample), QColor(Qt::red));
        QImage frame(600, 300, QImage::Format_RGB32);
        frame.fill(Qt::yellow);
        video->updateFrame(frame);
        QCOMPARE(colourAt(videoSample), QColor(Qt::yellow));
        QCOMPARE(colourAt(paddingSample), QColor(Qt::red));

        QImage cdgFrame(300, 216, QImage::Format_RGB32);
        cdgFrame.fill(Qt::yellow);
        video->updateFrame(cdgFrame);
        QCOMPARE(colourAt(frameCenter), QColor(Qt::yellow));
        QCOMPARE(colourAt(paddingSample), QColor(Qt::red));
        video->clearFrame();
        QCOMPARE(colourAt(videoSample), QColor(Qt::red));

        settings.setRotationDisplayBackgroundImage(directory.filePath("missing.png"));
        display.applySettings();
        QCOMPARE(colourAt({0, 0}), settings.rotationDisplayBackgroundColor());
        settings.setRotationDisplayBackgroundImage({});
        display.applySettings();
        QVERIFY(Settings().rotationDisplayBackgroundImage().isEmpty());
        QCOMPARE(colourAt({0, 0}), settings.rotationDisplayBackgroundColor());
        video->updateFrame(cdgFrame);
        QCOMPARE(colourAt(frameCenter), QColor(Qt::yellow));
        QCOMPARE(colourAt(paddingSample), settings.rotationDisplayBackgroundColor());
    }

    void singerRowsHaveAHeightLimitAndFitFullRotations()
    {
        Settings settings;
        settings.setRotationDisplaySingerCount(20);
        TableModelRotation model;
        auto rotation = singers().mid(0, 1);
        model.onRotationLoaded(rotation);
        RotationDisplay display(model);
        display.resize(640, 720);
        display.setDisplayVisible(true);
        const auto cardHeights = [&] {
            const auto image = display.grab().toImage();
            const auto background = settings.rotationDisplayBackgroundColor();
            QList<qreal> heights;
            int run = 0;
            for (int y = 0; y < image.height(); ++y) {
                if (image.pixelColor(image.width() / 2, y) != background) {
                    ++run;
                } else {
                    if (run > 4 * image.devicePixelRatio())
                        heights.append(run / image.devicePixelRatio());
                    run = 0;
                }
            }
            return heights;
        };
        const auto single = cardHeights();
        QCOMPARE(single.size(), 1);
        QVERIFY(single.first() <= 96);

        for (int i = 1; i < 20; ++i) {
            auto singer = rotation.first();
            singer.id += i;
            singer.position = i;
            rotation.append(singer);
        }
        model.onRotationLoaded(rotation);
        QTRY_COMPARE(display.singerNames().size(), 20);
        const auto full = cardHeights();
        QCOMPARE(full.size(), 20);
        QVERIFY(full.first() < single.first());
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
        const qreal margin = qBound(12.0, display.width() * 0.055, 64.0);
        const qreal panelWidth = display.width() - 2 * margin - margin * 0.75;
        QVERIFY(qAbs(video->width() - 0.7 * panelWidth) <= 2);
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
        QVERIFY(qAbs(video->width() - 0.7 * panelWidth) <= 2);
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
        Settings::setSafeStartupMode(true);
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
