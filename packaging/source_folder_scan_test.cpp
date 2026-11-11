// Exercise the real scanner against isolated folders and SQLite records.
#include "dbupdater.h"
#include <QApplication>
#include <QFutureWatcher>
#include <QSignalSpy>
#include <QTimer>
#include <QUuid>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <spdlog/sinks/null_sink.h>
#include <stdexcept>

class SourceFolderScanTest : public QObject
{
    Q_OBJECT
    QTemporaryDir temporary;
    QSqlDatabase database;

    void exec(const QString &sql)
    {
        QSqlQuery query(database);
        if (!query.exec(sql))
            throw std::runtime_error(query.lastError().text().toStdString());
    }
    void song(int id, const QString &path)
    {
        QSqlQuery query(database);
        query.prepare("INSERT INTO dbsongs (songid, path, discid) VALUES (?, ?, '')");
        query.addBindValue(id);
        query.addBindValue(path);
        if (!query.exec())
            throw std::runtime_error(query.lastError().text().toStdString());
    }
    QList<int> ids()
    {
        QSqlQuery query("SELECT songid FROM dbsongs ORDER BY songid", database);
        QList<int> result;
        while (query.next()) result.append(query.value(0).toInt());
        return result;
    }
    QString folder(const QString &name)
    {
        const auto path = temporary.filePath(name);
        if (!QDir().mkpath(path)) throw std::runtime_error("Cannot create fixture folder");
        return path;
    }
    void touch(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create fixture song");
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        spdlog::null_logger_mt("logger");
        QVERIFY(temporary.isValid());
    }
    void init()
    {
        database = QSqlDatabase::addDatabase("QSQLITE");
        database.setDatabaseName(temporary.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".sqlite"));
        QVERIFY(database.open());
        exec("CREATE TABLE dbsongs (songid INTEGER PRIMARY KEY, path TEXT UNIQUE, discid TEXT, artist TEXT, "
             "title TEXT, filename TEXT, duration INTEGER, searchstring TEXT)");
        exec("CREATE TABLE sourcedirs (path TEXT UNIQUE, pattern INT, custompattern INT)");
        exec("CREATE TABLE custompatterns (patternid INTEGER PRIMARY KEY, name TEXT, artistregex TEXT, "
             "artistcapturegrp INT, titleregex TEXT, titlecapturegrp INT, discidregex TEXT, discidcapturegrp INT)");
        exec("CREATE INDEX idx_path ON dbsongs(path)");
        exec("CREATE TABLE queuesongs (song INT)");
        exec("CREATE TABLE regularsongs (songid INT)");
    }
    void cleanup()
    {
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
    void literalFolderMatching_data()
    {
        QTest::addColumn<QString>("sourceName");
        QTest::addColumn<QString>("neighborName");
        QTest::newRow("underscore") << "Karaoke_A" << "KaraokeXA";
        QTest::newRow("percent") << "Karaoke%Set" << "KaraokeExtraSet";
        QTest::newRow("escape-and-unicode") << QString::fromUtf8("曲_%!'") << QString::fromUtf8("曲_Xmore!'");
        QTest::newRow("case") << "CaseSource" << "casesource";
        QTest::newRow("ordinary") << "Normal" << "NormalExtra";
    }
    void literalFolderMatching()
    {
        QFETCH(QString, sourceName);
        QFETCH(QString, neighborName);
        const auto source = folder(sourceName);
        folder(sourceName + "/nested");
        const auto present = source + "/present.mp4";
        touch(present);
        song(1, source + "/missing.mp4");
        song(2, source + "/nested/also-missing.mp4");
        song(3, present);
        song(4, temporary.filePath(neighborName) + "/unrelated.mp4");
        song(5, source + "Sibling/unrelated.mp4");
        song(6, temporary.filePath("disconnected/untouched.mp4"));
        exec("INSERT INTO queuesongs VALUES (4), (6)");
        exec("INSERT INTO regularsongs VALUES (4), (6)");
        DbUpdater updater;
        QVERIFY(updater.process({source + "/./"}, DbUpdater::PrepareForRemovalOfMissing));
        QCOMPARE(updater.getErrors(), QStringList());
        QCOMPARE(updater.missingFilesCount(), 2);
        QCOMPARE(ids(), QList<int>({1, 2, 3, 4, 5, 6})); // Scanning never deletes records.
        updater.removeMissingFilesFromDatabase(); // Explicit host approval only.
        QCOMPARE(ids(), QList<int>({3, 4, 5, 6}));
        QSqlQuery queues("SELECT COUNT(*) FROM queuesongs", database);
        QVERIFY(queues.next());
        QCOMPARE(queues.value(0).toInt(), 2);
        QSqlQuery regulars("SELECT COUNT(*) FROM regularsongs", database);
        QVERIFY(regulars.next());
        QCOMPARE(regulars.value(0).toInt(), 2);
    }
    void multipleSourcesAndWholeDatabaseLookup()
    {
        const auto first = folder("multiple_A");
        const auto second = folder("multiple%Z");
        folder("multiple_A/nested");
        song(1, first + "/missing.mp4");
        song(2, second + "/missing.mp4");
        song(3, temporary.filePath("multipleXA/neighbor.mp4"));
        song(4, temporary.filePath("unmounted/offline.mp4"));
        DbUpdater updater;
        QVERIFY(updater.process({first, second + "/", first + "/nested"}, DbUpdater::PrepareForRemovalOfMissing));
        QCOMPARE(updater.missingFilesCount(), 2);
        // Search the whole DB to find a moved song, while keeping unrelated
        // missing records out of the removal list.
        song(5, temporary.filePath("old-location/moved.mp4"));
        const auto moved = first + "/moved.mp4";
        touch(moved);
        QVERIFY(updater.process({first, second}, DbUpdater::FixMovedFilesSearchInWholeDB | DbUpdater::PrepareForRemovalOfMissing));
        QCOMPARE(updater.missingFilesCount(), 2);
        QSqlQuery query("SELECT path FROM dbsongs WHERE songid = 5", database);
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), moved);
        query.finish();
        updater.removeMissingFilesFromDatabase();
        QCOMPARE(ids(), QList<int>({3, 4, 5}));
    }
    void unavailableSourcesCannotSupplyRemovalCandidates()
    {
        const auto available = folder("available");
        const auto offline = temporary.filePath("never-mounted");
        song(1, available + "/missing.mp4");
        song(2, offline + "/offline.mp4");
        DbUpdater updater;
        QVERIFY(updater.process({available}, DbUpdater::PrepareForRemovalOfMissing));
        QCOMPARE(updater.missingFilesCount(), 1);
        QVERIFY(!updater.process({available, offline}, DbUpdater::FixMovedFilesSearchInWholeDB | DbUpdater::PrepareForRemovalOfMissing));
        QVERIFY(!updater.getErrors().isEmpty());
        QCOMPARE(updater.missingFilesCount(), 0); // Previous scan's candidates were cleared.
        updater.removeMissingFilesFromDatabase();
        QCOMPARE(ids(), QList<int>({1, 2}));
        QVERIFY(updater.process({}, DbUpdater::PrepareForRemovalOfMissing));
        QCOMPARE(updater.missingFilesCount(), 0);
    }
    void largeScanRunsInBackgroundAndUsesTheWorkerConnection()
    {
        const auto source = folder("worker-library");
        QSqlQuery pattern(database);
        QVERIFY(pattern.prepare("INSERT INTO sourcedirs(path, pattern) VALUES (?, ?)"));
        pattern.addBindValue(source);
        pattern.addBindValue(int(SourceDir::TA));
        QVERIFY(pattern.exec());
        const int count = 10000;
        for (int i = 0; i < count; ++i)
            touch(source + QString("/Title %1 - Artist.mp4").arg(i));
        const auto offline = temporary.filePath("offline-worker-source");
        song(999999, offline + "/missing.mp4");

        auto state = std::make_shared<DbUpdater::ScanState>();
        QFutureWatcher<DbUpdater::ScanResult> watcher;
        QSignalSpy done(&watcher, &QFutureWatcher<DbUpdater::ScanResult>::finished);
        int ticksDuringScan = 0;
        QTimer hostTimer;
        connect(&hostTimer, &QTimer::timeout, this, [&] {
            if (watcher.isRunning())
                ++ticksDuringScan;
        });
        hostTimer.start(1);
        watcher.setFuture(DbUpdater::scanAsync(database.databaseName(), {source, offline},
            DbUpdater::FixMovedFiles | DbUpdater::SkipUnavailableSources, state));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        const auto result = watcher.result();
        QVERIFY2(result.success, qPrintable(result.errors.join("\n")));
        QVERIFY(ticksDuringScan > 0);
        QVERIFY(result.missing.isEmpty());
        QSqlQuery query("SELECT COUNT(*) FROM dbsongs WHERE artist = 'Artist' AND title LIKE 'Title %'", database);
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), count);
        query.finish();
        QVERIFY(ids().contains(999999));
    }

    void cancelBackgroundScanWithoutChangingLibrary()
    {
        const auto source = folder("worker-cancelled");
        for (int i = 0; i < 4000; ++i)
            touch(source + QString("/Song - Artist - %1.mp4").arg(i));
        song(8, source + "/previously-saved.mp4");
        auto state = std::make_shared<DbUpdater::ScanState>();
        QFutureWatcher<DbUpdater::ScanResult> watcher;
        QSignalSpy done(&watcher, &QFutureWatcher<DbUpdater::ScanResult>::finished);
        QTimer cancelTimer;
        cancelTimer.setInterval(1);
        connect(&cancelTimer, &QTimer::timeout, this, [&] {
            QMutexLocker lock(&state->mutex);
            if (!state->progress.status.isEmpty())
                state->cancelled.store(true);
        });
        cancelTimer.start();
        watcher.setFuture(DbUpdater::scanAsync(database.databaseName(), {source},
            DbUpdater::PrepareForRemovalOfMissing, state));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QVERIFY(watcher.result().cancelled);
        QVERIFY(!watcher.result().success);
        QVERIFY(watcher.result().missing.isEmpty());
        QCOMPARE(ids(), QList<int>{8});
    }

    void failedWriteRollsBackMovesAndInserts()
    {
        const auto source = folder("rollback");
        const auto moved = source + "/Artist - Moved.mp4";
        const auto oldPath = source + "/old/Artist - Moved.mp4";
        touch(moved);
        touch(source + "/Artist - New.mp4");
        touch(source + "/Artist - Z rejected.mp4");
        song(9, oldPath);
        exec("CREATE TRIGGER reject_import BEFORE INSERT ON dbsongs "
             "WHEN NEW.path LIKE '%rejected.mp4' BEGIN SELECT RAISE(ABORT, 'rejected by test'); END");
        DbUpdater updater;
        QVERIFY(!updater.process({source}, DbUpdater::FixMovedFiles | DbUpdater::PrepareForRemovalOfMissing));
        QVERIFY(updater.getErrors().join(" ").contains("rejected by test"));
        QCOMPARE(updater.missingFilesCount(), 0);
        QCOMPARE(ids(), QList<int>{9});
        QSqlQuery query("SELECT path FROM dbsongs WHERE songid = 9", database);
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), oldPath);
    }

    void rechecksFilesAndDatabaseBeforeConfirmedRemoval()
    {
        const auto source = folder("confirm");
        const auto recovered = source + "/recovered.mp4";
        const auto missing = source + "/missing.mp4";
        const auto renamed = source + "/old-name.mp4";
        song(1, recovered);
        song(2, missing);
        song(3, renamed);
        DbUpdater updater;
        QVERIFY(updater.process({source}, DbUpdater::PrepareForRemovalOfMissing));
        QCOMPARE(updater.missingFilesCount(), 3);
        touch(recovered);
        exec("UPDATE dbsongs SET path = path || '.updated' WHERE songid = 3");
        QVERIFY(updater.removeMissingFilesFromDatabase());
        QCOMPARE(ids(), QList<int>({1, 3}));
    }

    void sourceDisappearsDuringScan()
    {
        const auto source = folder("removed-during-scan");
        song(1, source + "/missing.mp4");
        DbUpdater updater;
        connect(&updater, &DbUpdater::stateChanged, this, [&](const QString &state) {
            if (state == "Checking files against database...")
                QVERIFY(QDir().rmdir(source));
        });
        QVERIFY(!updater.process({source}, DbUpdater::PrepareForRemovalOfMissing));
        QCOMPARE(updater.missingFilesCount(), 0);
        updater.removeMissingFilesFromDatabase();
        QCOMPARE(ids(), QList<int>({1}));
    }
};

QTEST_MAIN(SourceFolderScanTest)
#include "source_folder_scan_test.moc"
