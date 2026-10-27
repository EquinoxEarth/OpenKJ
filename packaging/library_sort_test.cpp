// Uses a temporary SQLite library and test-mode settings, never the host library.
#include "models/tablemodelkaraokesongs.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {
void exec(const QString &sql)
{
    QSqlQuery query;
    if (!query.exec(sql))
        throw std::runtime_error(query.lastError().text().toStdString());
}

QList<int> ids(const TableModelKaraokeSongs &model)
{
    QList<int> result;
    for (int row = 0; row < model.rowCount({}); ++row)
        result.append(model.index(row, TableModelKaraokeSongs::COL_ID).data().toInt());
    return result;
}

// Hold the sole pool worker so tests can deterministically edit songs after
// searchExec captures its inputs, but before the job is allowed to read them.
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
            throw std::runtime_error("Cannot pause the worker");
        }
    }
    ~WorkerGate()
    {
        state->released.release();
        QThreadPool::globalInstance()->waitForDone();
    }
};
}

class LibrarySortTest : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QSqlDatabase database;
    int previousThreads = 0;
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        if (qEnvironmentVariableIntValue("OPENKJ_SORT_TEST_SONGS") == 600000)
            spdlog::stdout_logger_mt("logger");
        else
            spdlog::null_logger_mt("logger");
        QVERIFY(temporary.isValid());
        database = QSqlDatabase::addDatabase("QSQLITE");
        database.setDatabaseName(temporary.filePath("library.sqlite"));
        QVERIFY(database.open());
        exec("CREATE TABLE dbsongs (songid INTEGER PRIMARY KEY, artist TEXT, title TEXT, discid TEXT, "
             "duration INT DEFAULT 0, filename TEXT, path TEXT UNIQUE, searchstring TEXT, "
             "plays INT DEFAULT 0, lastplay TEXT)");
        previousThreads = QThreadPool::globalInstance()->maxThreadCount();
        QThreadPool::globalInstance()->setMaxThreadCount(1);
    }

    void init()
    {
        exec("DELETE FROM dbsongs");
        exec("INSERT INTO dbsongs VALUES "
             "(1,'Zebra','Alpha','D3',0,'c.ZIP','/songs/1','zebra alpha d3',3,'2024-01-01T00:00:00'),"
             "(2,'alpha','Zebra','d2',4000,'B.mp4','/songs/2','alpha zebra d2',1,'2022-01-01T00:00:00'),"
             "(3,'Alpha','Beta','d1',2000,'a.zip','/songs/3','alpha beta d1',4,'2023-01-01T00:00:00'),"
             "(4,'Alpha','Beta','d0',3000,'x.zip','/songs/4','alpha beta d0',2,'2025-01-01T00:00:00'),"
             "(5,'Åke','Écho','d4',1000,'É.zip','/songs/5','åke écho d4',0,'2021-01-01T00:00:00'),"
             "(6,'Hidden','Hidden','!!BAD!!',0,'hidden.zip','/songs/6','hidden',0,NULL)");
    }

    void allColumnsAndFiltering()
    {
        TableModelKaraokeSongs model;
        QSignalSpy loaded(&model, &TableModelKaraokeSongs::libraryLoadFinished);
        model.loadData();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QVERIFY(loaded[0][0].toBool());
        const QList<QList<int>> ascending = {
            {1,2,3,4,5}, {4,3,2,1,5}, {1,4,3,2,5}, {4,3,2,1,5},
            {3,2,1,4,5}, {1,5,3,4,2}, {5,2,4,1,3}, {5,2,3,1,4}
        };
        bool wrongThread = false;
        connect(&model, &QAbstractItemModel::modelReset, &model, [&] {
            wrongThread |= QThread::currentThread() != qApp->thread();
        }, Qt::DirectConnection);
        for (int column = 0; column < ascending.size(); ++column) {
            for (const auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
                model.sort(column, order);
                QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
                auto expected = ascending[column];
                if (order == Qt::DescendingOrder)
                    std::reverse(expected.begin(), expected.end());
                QCOMPARE(ids(model), expected);
            }
        }
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        model.search("alpha");
        model.sort(TableModelKaraokeSongs::COL_TITLE, Qt::AscendingOrder);
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({1,4,3,2}));
        QVERIFY(!wrongThread);
    }

    void latestRequestAndRecordEditsWin()
    {
        TableModelKaraokeSongs model;
        QSignalSpy loaded(&model, &TableModelKaraokeSongs::libraryLoadFinished);
        model.loadData();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        auto retained = model.index(0, 0).data(Qt::UserRole).value<std::shared_ptr<okj::KaraokeSong>>();
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        {
            WorkerGate gate;
            model.sort(TableModelKaraokeSongs::COL_FILENAME, Qt::DescendingOrder);
            QTest::qWait(150);
            model.search("alpha");
            model.sort(TableModelKaraokeSongs::COL_ARTIST, Qt::DescendingOrder);
            exec("UPDATE dbsongs SET artist='Alpha',title='Aardvark',searchstring='alpha aardvark' WHERE songid=1");
            QVERIFY(model.refreshSong(1));
            model.sort(TableModelKaraokeSongs::COL_TITLE, Qt::AscendingOrder);
            QTest::qWait(150);
            QCOMPARE(reset.count(), 0);
        }
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({1,4,3,2}));
        QCOMPARE(&model.getSong(1), retained.get());
        QCOMPARE(model.getSong(1).title, QString("Aardvark"));
        QCOMPARE(model.getIdForPath("/songs/1"), 1);

        // An inserted record must be placed by its sort keys, not storage order.
        reset.clear();
        exec("INSERT INTO dbsongs VALUES (7,'Alpha','Aardvark','d0',500,'new.zip','/songs/7','alpha aardvark',0,NULL)");
        QVERIFY(model.refreshSong(7));
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({7,1,4,3,2}));
    }

    void updatesAfterCaptureSurvive()
    {
        TableModelKaraokeSongs model;
        QSignalSpy loaded(&model, &TableModelKaraokeSongs::libraryLoadFinished);
        model.loadData();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        {
            WorkerGate gate;
            exec("UPDATE dbsongs SET title='Changed',searchstring='zebra changed' WHERE songid=1");
            QVERIFY(model.refreshSong(1));
            model.sort(TableModelKaraokeSongs::COL_TITLE, Qt::AscendingOrder);
            QTest::qWait(150);
            // These fields do not affect a title sort; its result may still be applied.
            model.setSongDuration("/songs/1", 1500);
            for (int count = 0; count < 5; ++count)
                model.updateSongHistory(5);
        }
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(model.getSong(1).duration, 1500);
        QCOMPARE(model.getSong(5).plays, 5);
        reset.clear();
        model.sort(TableModelKaraokeSongs::COL_DURATION, Qt::AscendingOrder);
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({5,1,3,4,2}));
        reset.clear();
        model.sort(TableModelKaraokeSongs::COL_PLAYS, Qt::DescendingOrder);
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({5,3,1,4,2}));

        reset.clear();
        {
            WorkerGate gate;
            model.sort(TableModelKaraokeSongs::COL_FILENAME, Qt::AscendingOrder);
            QTest::qWait(150);
            model.markSongBad("/songs/2");
        }
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({3,1,4,5}));
    }

    void durationChangeSupersedesQueuedNumericSort()
    {
        TableModelKaraokeSongs model;
        QSignalSpy loaded(&model, &TableModelKaraokeSongs::libraryLoadFinished);
        model.loadData();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        {
            WorkerGate gate;
            model.sort(TableModelKaraokeSongs::COL_DURATION, Qt::DescendingOrder);
            QTest::qWait(150);
            model.setSongDuration("/songs/1", 9000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({1,2,4,3,5}));
    }

    void updatesDuringInitialLoad()
    {
        TableModelKaraokeSongs model;
        QSignalSpy loaded(&model, &TableModelKaraokeSongs::libraryLoadFinished);
        {
            WorkerGate gate;
            model.loadData();
            model.sort(TableModelKaraokeSongs::COL_DURATION, Qt::DescendingOrder);
            model.setSongDuration("/songs/1", 9000);
            model.updateSongHistory(5);
            exec("UPDATE dbsongs SET title='Changed',searchstring='alpha changed' WHERE songid=3");
            QVERIFY(model.refreshSong(3));
        }
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QCOMPARE(ids(model), QList<int>({1,2,4,3,5}));
        QCOMPARE(model.getSong(1).duration, 9000);
        QCOMPARE(model.getSong(5).plays, 1);
        QCOMPARE(model.getSong(3).title, QString("Changed"));
    }

    void destructionWithQueuedSort()
    {
        auto model = std::make_unique<TableModelKaraokeSongs>();
        QSignalSpy loaded(model.get(), &TableModelKaraokeSongs::libraryLoadFinished);
        model->loadData();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        {
            WorkerGate gate;
            model->sort(TableModelKaraokeSongs::COL_TITLE, Qt::DescendingOrder);
            QTest::qWait(150);
            model.reset();
        }
        QTest::qWait(20);
    }

    void largeLibrary()
    {
        exec("DELETE FROM dbsongs");
        const int count = qEnvironmentVariableIntValue("OPENKJ_SORT_TEST_SONGS") == 600000 ? 600000 : 100000;
        exec(QString("WITH RECURSIVE n(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM n WHERE id<@COUNT@) "
             "INSERT INTO dbsongs SELECT id, printf('Artist %06d',(id*7919)%@COUNT@), "
             "printf('Title %06d',(id*3571)%@COUNT@), printf('Disc %06d',id), id*10, "
             "printf('File %06d.zip',@COUNT@+1-id), printf('/songs/%d',id), 'artist title', id%100, "
             "'2024-01-01T00:00:00Z' FROM n").replace("@COUNT@", QString::number(count)));
        TableModelKaraokeSongs model;
        QSignalSpy loaded(&model, &TableModelKaraokeSongs::libraryLoadFinished);
        model.loadData();
        const int timeout = count == 600000 ? 30000 : 20000;
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, timeout);
        QCOMPARE(model.rowCount({}), count);
        int heartbeats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, &model, [&] { ++heartbeats; });
        heartbeat.start();
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QElapsedTimer timer;
        timer.start();
        model.sort(TableModelKaraokeSongs::COL_FILENAME, Qt::AscendingOrder);
        const auto requestNs = timer.nsecsElapsed();
        QTRY_COMPARE_WITH_TIMEOUT(reset.count(), 1, timeout);
        QVERIFY(heartbeats > 0);
        QCOMPARE(model.index(0, 0).data().toInt(), count);
        QCOMPARE(model.index(count - 1, 0).data().toInt(), 1);
        qInfo("%d songs: sort request %.3f ms; completion %lld ms; UI heartbeats %d",
              count, requestNs / 1000000.0, static_cast<long long>(timer.elapsed()), heartbeats);
    }

    void cleanupTestCase()
    {
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(previousThreads);
        database.close();
        database = {};
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
};

QTEST_MAIN(LibrarySortTest)
#include "library_sort_test.moc"
