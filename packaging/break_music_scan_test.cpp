// Scans temporary media and a temporary database; never opens the host library.
#include "bmdbdialog.h"
#include "bmdbupdatethread.h"

#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <stdexcept>

namespace {
void exec(const QString &sql)
{
    QSqlQuery query;
    if (!query.exec(sql))
        throw std::runtime_error(query.lastError().text().toStdString());
}
QVariant scalar(const QString &sql)
{
    QSqlQuery query(sql);
    if (!query.next())
        throw std::runtime_error(query.lastError().text().toStdString());
    return query.value(0);
}
class WorkerGate {
    struct State { QSemaphore entered, released; };
    std::shared_ptr<State> state = std::make_shared<State>();
public:
    WorkerGate()
    {
        QThreadPool::globalInstance()->start([held = state] {
            held->entered.release();
            held->released.acquire();
        });
        if (!state->entered.tryAcquire(1, 10000)) {
            state->released.release();
            throw std::runtime_error("Cannot pause worker");
        }
    }
    ~WorkerGate()
    {
        state->released.release();
        QThreadPool::globalInstance()->waitForDone();
    }
};
}

class BreakMusicScanTest : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QSqlDatabase database;
    int oldThreads = 0;
    QString root, fixture;

    void addMedia(const QString &relative)
    {
        const auto destination = QDir(root).filePath(relative);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(fixture, destination))
            throw std::runtime_error("Cannot create scan fixture");
    }
    void addFiles(int count)
    {
        for (int file = 0; file < count; ++file)
            addMedia(QString("song%1.wav").arg(file));
    }
    void createWave()
    {
        {
            QFile file(fixture);
            if (!file.open(QIODevice::WriteOnly))
                throw std::runtime_error("Cannot create test WAV");
            QDataStream wave(&file);
            wave.setByteOrder(QDataStream::LittleEndian);
            wave.writeRawData("RIFF", 4);
            wave << quint32(32036);
            wave.writeRawData("WAVEfmt ", 8);
            wave << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000)
                 << quint16(2) << quint16(16);
            wave.writeRawData("data", 4);
            wave << quint32(32000);
            file.write(QByteArray(32000, '\0'));
        }
#ifdef Q_OS_WIN
        const auto filename = fixture.toStdWString();
        TagLib::FileRef audio(filename.c_str());
#else
        const auto filename = QFile::encodeName(fixture);
        TagLib::FileRef audio(filename.constData());
#endif
        if (audio.isNull() || !audio.tag())
            throw std::runtime_error("Cannot tag test WAV");
        audio.tag()->setArtist(TagLib::String("O'Connor & \"Night\" 夜", TagLib::String::UTF8));
        audio.tag()->setTitle(TagLib::String("Don't Stop", TagLib::String::UTF8));
        if (!audio.save())
            throw std::runtime_error("Cannot save test tags");
    }
    void addKnown(const QString &path, int id)
    {
        QSqlQuery query;
        query.prepare("INSERT INTO bmsongs VALUES (:id,'Preserved','Original',:path,'legacy-filename',123,'preserved original')");
        query.bindValue(":id", id);
        query.bindValue(":path", path);
        if (!query.exec())
            throw std::runtime_error(query.lastError().text().toStdString());
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        spdlog::null_logger_mt("logger");
        QVERIFY(temporary.isValid());
        database = QSqlDatabase::addDatabase("QSQLITE");
        database.setDatabaseName(temporary.filePath("library.sqlite"));
        QVERIFY(database.open());
        exec("CREATE TABLE bmsongs (songid INTEGER PRIMARY KEY, artist TEXT, title TEXT, path TEXT UNIQUE, "
             "filename TEXT, duration INT, searchstring TEXT)");
        exec("CREATE TABLE bmsrcdirs (path TEXT)");
        exec("CREATE TABLE bmplaylists (playlistid INT, title TEXT)");
        exec("CREATE TABLE bmplsongs (playlist INT, song INT)");
        root = temporary.filePath("zzz-source");
        fixture = temporary.filePath("fixture.wav");
        createWave();
        oldThreads = QThreadPool::globalInstance()->maxThreadCount();
        QThreadPool::globalInstance()->setMaxThreadCount(1);
    }
    void init()
    {
        exec("DROP TRIGGER IF EXISTS reject_batch");
        exec("DELETE FROM bmsongs");
        exec("DELETE FROM bmsrcdirs");
        exec("DELETE FROM bmplaylists");
        exec("DELETE FROM bmplsongs");
        QVERIFY(QDir(root).removeRecursively());
        QVERIFY(QDir().mkpath(root));
    }

    void metadataOverlapsAndDisconnectedMedia()
    {
        addMedia("known.wav");
        addMedia(QStringLiteral("nested/Zoë 夜.WAV"));
        QFile ignored(QDir(root).filePath("ignored.txt"));
        QVERIFY(ignored.open(QIODevice::WriteOnly));
        ignored.close();
        addKnown(QDir(root).filePath("known.wav"), 101);
        const auto missing = temporary.filePath("disconnected");
        addKnown(missing + "/old.mp3", 202);
        exec("INSERT INTO bmplaylists VALUES (7,'Keep playing')");
        exec("INSERT INTO bmplsongs VALUES (7,101)");
        BmDbUpdater updater;
        QSignalSpy finished(&updater, &BmDbUpdater::finished);
        bool wrongThread = false;
        connect(&updater, &BmDbUpdater::progressChanged, &updater, [&](auto, auto) {
            wrongThread |= QThread::currentThread() != qApp->thread();
        }, Qt::DirectConnection);
        QVERIFY(updater.start(database.databaseName(), {root, root + "/nested", root, missing, ""}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished[0][0].value<BmDbUpdateResult>();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!result.cancelled && !updater.isRunning() && !wrongThread);
        QCOMPARE(result.total, 2);
        QCOMPARE(result.processed, 2);
        QCOMPARE(result.added, 1);
        QCOMPARE(result.existing, 1);
        QCOMPARE(result.unavailablePaths.size(), 2);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 3);
        QCOMPARE(scalar("SELECT artist FROM bmsongs WHERE songid=101").toString(), QString("Preserved"));
        QCOMPARE(scalar("SELECT filename FROM bmsongs WHERE songid=101").toString(), QString("legacy-filename"));
        QCOMPARE(scalar("SELECT title FROM bmsongs WHERE songid=202").toString(), QString("Original"));
        QCOMPARE(scalar("SELECT filename FROM bmsongs WHERE songid NOT IN (101,202)").toString(),
                 QDir(root).filePath(QStringLiteral("nested/Zoë 夜.WAV")));
        QCOMPARE(scalar("SELECT artist FROM bmsongs WHERE songid NOT IN (101,202)").toString(), QStringLiteral("O'Connor & \"Night\" 夜"));
        QCOMPARE(scalar("SELECT duration FROM bmsongs WHERE songid NOT IN (101,202)").toInt(), 2);
        QCOMPARE(scalar("SELECT song FROM bmplsongs WHERE playlist=7").toInt(), 101);
        QCOMPARE(QSqlDatabase::connectionNames().size(), 1);
    }

    void batchFailurePreservesCommittedSongs()
    {
        addFiles(40);
        exec("CREATE TRIGGER reject_batch BEFORE INSERT ON bmsongs WHEN (SELECT COUNT(*) FROM bmsongs)>=32 "
             "BEGIN SELECT RAISE(ABORT,'test failed batch'); END");
        BmDbUpdater updater;
        QSignalSpy finished(&updater, &BmDbUpdater::finished);
        QVERIFY(updater.start(database.databaseName(), {root}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished[0][0].value<BmDbUpdateResult>();
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(result.added, 32);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 32);
        QCOMPARE(QSqlDatabase::connectionNames().size(), 1);
    }

    void cancellationAndShutdown()
    {
        addFiles(40);
        exec("BEGIN IMMEDIATE");
        const auto rollback = qScopeGuard([] { QSqlDatabase::database().rollback(); });
        BmDbUpdater updater;
        QSignalSpy finished(&updater, &BmDbUpdater::finished);
        bool heartbeat = false;
        QTimer::singleShot(50, &updater, [&] { heartbeat = true; updater.cancel(); });
        QVERIFY(updater.start(database.databaseName(), {root}));
        QVERIFY(!updater.start(database.databaseName(), {root}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished[0][0].value<BmDbUpdateResult>();
        QVERIFY(heartbeat && result.cancelled && result.error.isEmpty());
        QCOMPARE(result.added, 0);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 0);
        {
            WorkerGate gate;
            auto destroyed = std::make_unique<BmDbUpdater>();
            QVERIFY(destroyed->start(database.databaseName(), {root}));
            destroyed.reset();
        }
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 0);
    }

    void missingInputsAndThrottledProgress()
    {
        BmDbUpdater updater;
        QVERIFY(!updater.start(database.databaseName(), {}));
        QSignalSpy finished(&updater, &BmDbUpdater::finished);
        const auto missingDb = temporary.filePath("missing.sqlite");
        QVERIFY(updater.start(missingDb, {root}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished[0][0].value<BmDbUpdateResult>().error.isEmpty());
        QVERIFY(!QFile::exists(missingDb));
        finished.clear();
        exec("BEGIN");
        for (int index = 0; index < 400; ++index) {
            const auto path = QDir(root).filePath(QString("known%1.mp3").arg(index));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            addKnown(path, index + 1);
        }
        exec("COMMIT");
        QSignalSpy progress(&updater, &BmDbUpdater::progressChanged);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(updater.start(database.databaseName(), {root}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished[0][0].value<BmDbUpdateResult>();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.existing, 400);
        QCOMPARE(result.added, 0);
        QVERIFY(progress.count() <= elapsed.elapsed() / 80 + 3);
    }

    void dialogUpdateAllFetchesEverySource()
    {
        addMedia("one.wav");
        QSqlQuery missing;
        missing.prepare("WITH RECURSIVE n(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM n WHERE id<300) "
                        "INSERT INTO bmsrcdirs SELECT :prefix||id FROM n");
        missing.bindValue(":prefix", temporary.filePath("a-missing-"));
        QVERIFY(missing.exec());
        QSqlQuery insert;
        insert.prepare("INSERT INTO bmsrcdirs VALUES (:path)");
        insert.bindValue(":path", root);
        QVERIFY(insert.exec());
        BmDbDialog dialog;
        QSignalSpy about(&dialog, &BmDbDialog::bmDbAboutToUpdate);
        QSignalSpy finished(&dialog, &BmDbDialog::bmDbUpdated);
        // Avoid a completion message box while still exercising the real finish handler.
        connect(&dialog, &BmDbDialog::bmDbUpdated, &dialog, &QDialog::hide);
        dialog.show();
        {
            WorkerGate gate;
            QVERIFY(QMetaObject::invokeMethod(&dialog, "pushButtonUpdateAllClicked", Qt::DirectConnection));
            QVERIFY(dialog.isUpdating());
            QCOMPARE(about.count(), 1);
            QVERIFY(!dialog.findChild<QGroupBox *>("groupBox")->isEnabled());
            auto *progress = dialog.findChild<DlgDbUpdate *>();
            QVERIFY(progress && !progress->isModal());
            bool heartbeat = false;
            QTimer::singleShot(0, &dialog, [&] { heartbeat = true; });
            QTest::qWait(20);
            QVERIFY(heartbeat);
            QVERIFY(QMetaObject::invokeMethod(&dialog, "pushButtonUpdateAllClicked", Qt::DirectConnection));
            QCOMPARE(about.count(), 1);
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!dialog.isUpdating());
        QVERIFY(dialog.findChild<QGroupBox *>("groupBox")->isEnabled());
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 1);
    }

    void dialogSelectedSourceCanCancelAndRestart()
    {
        addMedia("one.wav");
        QSqlQuery insert;
        insert.prepare("INSERT INTO bmsrcdirs VALUES (:path)");
        insert.bindValue(":path", root);
        QVERIFY(insert.exec());
        BmDbDialog dialog;
        connect(&dialog, &BmDbDialog::bmDbUpdated, &dialog, &QDialog::hide);
        auto *updater = dialog.findChild<BmDbUpdater *>();
        QVERIFY(updater);
        QSignalSpy finished(updater, &BmDbUpdater::finished);
        dialog.show();
        dialog.findChild<QTableView *>("tableViewPaths")->selectRow(0);
        {
            WorkerGate gate;
            QVERIFY(QMetaObject::invokeMethod(&dialog, "pushButtonUpdateClicked", Qt::DirectConnection));
            QVERIFY(dialog.isUpdating());
            auto *cancel = dialog.findChild<QPushButton *>("pushButtonCancelUpdate");
            QVERIFY(cancel && cancel->isEnabled());
            cancel->click();
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(finished[0][0].value<BmDbUpdateResult>().cancelled);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 0);
        QVERIFY(!dialog.isUpdating());
        QVERIFY(QMetaObject::invokeMethod(&dialog, "pushButtonUpdateClicked", Qt::DirectConnection));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 10000);
        QVERIFY(!finished[1][0].value<BmDbUpdateResult>().cancelled);
        QCOMPARE(finished[1][0].value<BmDbUpdateResult>().added, 1);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 1);
    }

    void cleanupTestCase()
    {
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(oldThreads);
        database.close();
        database = {};
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
};

QTEST_MAIN(BreakMusicScanTest)
#include "break_music_scan_test.moc"
