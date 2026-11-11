// Delayed real SQLite replies, isolated from the host's library and settings.
#include "databaseworker.h"
#include "models/tablemodelqueuesongs.h"
#include "models/tablemodelrotation.h"

#include <QApplication>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <spdlog/sinks/null_sink.h>
#include <memory>
#include <stdexcept>

class QueueLoadingTest : public QObject
{
    Q_OBJECT
    struct Reply {
        int singerId;
        quint64 requestId;
        QList<okj::QueueSong> songs;
    };
    QTemporaryDir temporary;
    QSqlDatabase database;
    std::unique_ptr<TableModelKaraokeSongs> library;
    std::unique_ptr<TableModelQueueSongs> queue;
    std::unique_ptr<DatabaseWorker> worker;
    QList<Reply> replies;

    void exec(const QString &sql)
    {
        QSqlQuery query(database);
        if (!query.exec(sql))
            throw std::runtime_error(query.lastError().text().toStdString());
    }

    QVariant value(const QString &sql)
    {
        QSqlQuery query(database);
        if (!query.exec(sql) || !query.next())
            throw std::runtime_error(query.lastError().text().toStdString());
        return query.value(0);
    }

    void deliver(int index)
    {
        const auto reply = replies.at(index);
        QMetaObject::invokeMethod(queue.get(), [this, reply] {
            queue->onQueueLoaded(reply.singerId, reply.requestId, reply.songs);
        }, Qt::QueuedConnection);
        QCoreApplication::sendPostedEvents(queue.get(), QEvent::MetaCall);
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
        replies.clear();
        database = QSqlDatabase::addDatabase("QSQLITE");
        const auto path = temporary.filePath("queues.sqlite");
        QFile::remove(path);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        exec("CREATE TABLE rotationsingers (singerid INTEGER PRIMARY KEY, name TEXT, "
             "position INT, regular INT, regularid INT, addts TEXT)");
        exec("CREATE TABLE dbsongs (songid INTEGER PRIMARY KEY, artist TEXT, title TEXT, discid TEXT, "
             "duration INT, path TEXT, filename TEXT, searchstring TEXT, plays INT, lastplay TEXT)");
        exec("CREATE TABLE queuesongs (qsongid INTEGER PRIMARY KEY, singer INT, song INT, "
             "artist INT, title INT, discid INT, path INT, keychg INT, played INT, position INT)");
        exec("INSERT INTO rotationsingers VALUES (1, 'A', 0, 0, -1, ''), "
             "(2, 'B', 1, 0, -1, ''), (3, 'Empty', 2, 0, -1, '')");
        exec("INSERT INTO dbsongs VALUES (1, 'Artist A', 'Song A', 'A', 1000, '/a', 'a', 'a', 0, ''), "
             "(2, 'Artist B', 'Song B', 'B', 2000, '/b', 'b', 'b', 0, ''), "
             "(3, 'Artist C', 'Song C', 'C', 3000, '/c', 'c', 'c', 0, '')");
        exec("INSERT INTO queuesongs VALUES (11, 1, 1, 1, 1, 1, 1, 0, 0, 0), "
             "(21, 2, 2, 2, 2, 2, 2, 0, 0, 0), (22, 2, 3, 3, 3, 3, 3, 0, 0, 1)");
        library = std::make_unique<TableModelKaraokeSongs>();
        queue = std::make_unique<TableModelQueueSongs>(*library);
        worker = std::make_unique<DatabaseWorker>(path);
        worker->init();
        connect(queue.get(), &TableModelQueueSongs::requestLoadQueue, worker.get(), &DatabaseWorker::loadQueue);
        connect(worker.get(), &DatabaseWorker::queueLoaded, this,
                [this](int singerId, quint64 requestId, const QList<okj::QueueSong> &songs) {
            replies.append({singerId, requestId, songs});
        });
        connect(queue.get(), &TableModelQueueSongs::requestCommitQueue, worker.get(), &DatabaseWorker::commitQueue);
        connect(queue.get(), &TableModelQueueSongs::requestSetQueueSongKey, worker.get(), &DatabaseWorker::setQueueSongKey);
        connect(queue.get(), &TableModelQueueSongs::requestSetQueueSongPlayed, worker.get(), &DatabaseWorker::setQueueSongPlayed);
        connect(queue.get(), &TableModelQueueSongs::requestRemoveAllQueueSongs, worker.get(), &DatabaseWorker::removeAllQueueSongs);
        connect(queue.get(), &TableModelQueueSongs::requestAddSongToQueue, worker.get(), &DatabaseWorker::addSongToQueue);
    }

    void cleanup()
    {
        queue.reset();
        library.reset();
        worker.reset();
        QSqlDatabase::removeDatabase("openkj_background");
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }

    void switchingClearsSelectionAndRejectsDelayedReplies()
    {
        queue->loadSinger(1);
        QCOMPARE(replies.size(), 1);
        QCOMPARE(replies[0].singerId, 1);
        deliver(0);
        QVERIFY(queue->isQueueReady());
        QPersistentModelIndex oldIndex(queue->index(0, 0));
        QItemSelectionModel selection(queue.get());
        selection.select(oldIndex, QItemSelectionModel::Select);

        queue->loadSinger(1); // Hold an older A request while switching to B.
        queue->loadSinger(2);
        QVERIFY(!oldIndex.isValid());
        QVERIFY(!selection.hasSelection());
        QCOMPARE(queue->rowCount(), 0);
        QVERIFY(!queue->isQueueReady());
        QVERIFY(replies[1].requestId != replies[2].requestId);
        QSignalSpy changed(queue.get(), &TableModelQueueSongs::queueModified);
        deliver(1);
        QCOMPARE(queue->rowCount(), 0);
        QCOMPARE(changed.size(), 0);
        deliver(2);
        QCOMPARE(queue->rowCount(), 2);
        QCOMPARE(queue->index(0, TableModelQueueSongs::COL_ID).data().toInt(), 21);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed[0][0].toInt(), 2);
        deliver(1); // A arriving after B must also be harmless.
        QCOMPARE(queue->rowCount(), 2);
        QCOMPARE(changed.size(), 1);

        queue->setKey(11, 5); // Stale context-menu/dialog IDs are not members of B.
        queue->setPlayed(11);
        queue->remove(11);
        queue->moveSongId(11, 0);
        QCOMPARE(queue->rowCount(), 2);
        queue->setKey(21, -2);
        queue->setPlayed(21);
        queue->move(0, 1);
        QCOMPARE(value("SELECT keychg FROM queuesongs WHERE qsongid = 21").toInt(), -2);
        QCOMPARE(value("SELECT played FROM queuesongs WHERE qsongid = 21").toInt(), 1);
        QCOMPARE(value("SELECT position FROM queuesongs WHERE qsongid = 21").toInt(), 1);
        queue->remove(22);
        queue->commitChanges();
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE singer = 2").toInt(), 1);
        queue->removeAll();
        deliver(2); // Duplicate delivery must not undo edits made after loading.
        QCOMPARE(queue->rowCount(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE singer = 2").toInt(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE singer = 1").toInt(), 1);
        QCOMPARE(value("SELECT keychg + played + position FROM queuesongs WHERE qsongid = 11").toInt(), 0);
    }

    void requestIdentityHandlesABAEmptyAndDeselection()
    {
        queue->loadSinger(1);
        queue->loadSinger(2);
        queue->loadSinger(1);
        deliver(0); // Same singer, obsolete request.
        QVERIFY(!queue->isQueueReady());
        deliver(1);
        QVERIFY(!queue->isQueueReady());
        deliver(2);
        QVERIFY(queue->isQueueReady());
        QCOMPARE(queue->rowCount(), 1);
        queue->loadSinger(3);
        QCOMPARE(replies[3].singerId, 3);
        QVERIFY(replies[3].songs.empty());
        deliver(3);
        QVERIFY(queue->isQueueReady()); // An empty queue is a successfully loaded queue.
        QCOMPARE(queue->rowCount(), 0);
        deliver(2);
        QCOMPARE(queue->rowCount(), 0);
        QVERIFY(library->refreshSong(1));
        queue->songAddSlot(1, 3, -3);
        QCOMPARE(queue->rowCount(), 1);
        QCOMPARE(value("SELECT keychg FROM queuesongs WHERE singer = 3").toInt(), -3);
        queue->loadSinger(2);
        queue->loadSinger(-1);
        deliver(4);
        QCOMPARE(queue->getSingerId(), -1);
        QVERIFY(!queue->isQueueReady());
        QCOMPARE(queue->rowCount(), 0);
    }

    void pendingQueueCannotBeEditedOrDragged()
    {
        queue->loadSinger(1);
        deliver(0);
        queue->loadSinger(2);
        QSignalSpy commits(queue.get(), &TableModelQueueSongs::requestCommitQueue);
        QSignalSpy keys(queue.get(), &TableModelQueueSongs::requestSetQueueSongKey);
        QSignalSpy played(queue.get(), &TableModelQueueSongs::requestSetQueueSongPlayed);
        QSignalSpy clears(queue.get(), &TableModelQueueSongs::requestRemoveAllQueueSongs);
        QCOMPARE(queue->add(1), -1);
        queue->insert(1, 0);
        queue->move(0, 1);
        queue->moveSongId(11, 0);
        queue->remove(11);
        queue->setKey(11, 4);
        queue->setPlayed(11);
        queue->removeAll();
        queue->commitChanges();
        queue->sort(TableModelQueueSongs::COL_TITLE, Qt::AscendingOrder);
        QMimeData drop;
        drop.setData("integer/songid", "1");
        QVERIFY(!queue->canDropMimeData(&drop, Qt::CopyAction, 0, 0, {}));
        QVERIFY(!queue->dropMimeData(&drop, Qt::CopyAction, 0, 0, {}));
        QCOMPARE(queue->flags({}), Qt::NoItemFlags);
        QCOMPARE(commits.size() + keys.size() + played.size() + clears.size(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs").toInt(), 3);
        deliver(1);
        QVERIFY(queue->isQueueReady());
        QVERIFY(queue->flags(queue->index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
        // A drag started in A must not reorder B, even after B has loaded.
        QMimeData staleDrag;
        staleDrag.setData("text/queueitems", "[11]");
        QVERIFY(!queue->dropMimeData(&staleDrag, Qt::MoveAction, 0, 0, {}));
        QCOMPARE(commits.size(), 0);
    }

    void explicitAddAndPlaybackRemainSafeWhileLoading()
    {
        queue->loadSinger(2);
        queue->songAddSlot(1, 2, 3);
        QCOMPARE(replies.size(), 2); // Reload is issued after the targeted append.
        deliver(0);
        QVERIFY(!queue->isQueueReady());
        deliver(1);
        QCOMPARE(queue->rowCount(), 3);
        QCOMPARE(queue->index(2, TableModelQueueSongs::COL_KEY).data().toString(), "+3");
        queue->setPlayedForSinger(1, 11); // Playback does not depend on selection.
        QCOMPARE(value("SELECT played FROM queuesongs WHERE qsongid = 11").toInt(), 1);
        QVERIFY(!queue->getPlayed(21));
        worker->setQueueSongKey(2, 11, 5); // Worker also enforces singer membership.
        worker->setQueueSongPlayed(2, 11, false);
        QCOMPARE(value("SELECT keychg FROM queuesongs WHERE qsongid = 11").toInt(), 0);
        QCOMPARE(value("SELECT played FROM queuesongs WHERE qsongid = 11").toInt(), 1);

        queue->loadSinger(2);
        queue->setPlayedForSinger(2, 21);
        deliver(2); // Snapshot taken before marking played is obsolete.
        QVERIFY(!queue->isQueueReady());
        deliver(3);
        QVERIFY(queue->getPlayed(21));
        QVERIFY(library->refreshSong(1));
        const auto added = queue->add(1);
        QVERIFY(added > 0);
        queue->setKey(added, -1);
        QCOMPARE(value(QString("SELECT singer FROM queuesongs WHERE qsongid = %1").arg(added)).toInt(), 2);
        QCOMPARE(value(QString("SELECT keychg FROM queuesongs WHERE qsongid = %1").arg(added)).toInt(), -1);
    }
    void staleRotationCannotEraseDelayedSingerAddition()
    {
        TableModelRotation rotation;
        QList<QList<okj::RotationSinger>> rotations;
        connect(worker.get(), &DatabaseWorker::rotationLoaded, this,
                [&](const QList<okj::RotationSinger> &singers) { rotations.append(singers); });
        connect(&rotation, &TableModelRotation::requestCommitRotation, worker.get(), &DatabaseWorker::commitRotation);
        QSignalSpy errors(worker.get(), &DatabaseWorker::errorOccurred);
        worker->loadRotation();
        rotation.onRotationLoaded(rotations.last());
        worker->addSinger("Delayed", TableModelRotation::ADD_BOTTOM, false);
        // The GUI still shows A/B/Empty, because the add reply has not arrived.
        rotation.singerMove(1, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(value("SELECT COUNT(*) FROM rotationsingers").toInt(), 4);
        QCOMPARE(value("SELECT position FROM rotationsingers WHERE singerid = 1").toInt(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM rotationsingers WHERE name = 'Delayed'").toInt(), 1);
        rotation.onRotationLoaded(rotations.last());
        rotation.singerMove(1, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(value("SELECT position FROM rotationsingers WHERE singerid = 2").toInt(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM rotationsingers").toInt(), 4);
        disconnect(worker.get(), nullptr, this, nullptr);
    }

    void rotationFailureRollsBackAndPreservesMetadata()
    {
        TableModelRotation rotation;
        connect(worker.get(), &DatabaseWorker::rotationLoaded, &rotation, &TableModelRotation::onRotationLoaded);
        connect(&rotation, &TableModelRotation::requestCommitRotation, worker.get(), &DatabaseWorker::commitRotation);
        QSignalSpy errors(worker.get(), &DatabaseWorker::errorOccurred);
        QSignalSpy loaded(worker.get(), &DatabaseWorker::rotationLoaded);
        exec("UPDATE rotationsingers SET regularid = 42 WHERE singerid = 1");
        worker->loadRotation();
        exec("CREATE TRIGGER fail_second_position BEFORE UPDATE OF position ON rotationsingers "
             "WHEN OLD.singerid = 1 BEGIN SELECT RAISE(ABORT, 'injected rotation write failure'); END");
        loaded.clear();
        rotation.singerMove(1, 0); // First UPDATE succeeds; second must roll everything back.
        QCOMPARE(errors.size(), 1);
        QCOMPARE(loaded.size(), 0);
        QCOMPARE(value("SELECT position FROM rotationsingers WHERE singerid = 2").toInt(), 1);
        QCOMPARE(value("SELECT position FROM rotationsingers WHERE singerid = 1").toInt(), 0);
        exec("DROP TRIGGER fail_second_position");
        worker->loadRotation();
        rotation.singerMove(1, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(value("SELECT regularid FROM rotationsingers WHERE singerid = 1").toInt(), 42);
        // Deleting a singer and its queue is atomic too.
        exec("CREATE TRIGGER fail_singer_delete BEFORE DELETE ON rotationsingers "
             "BEGIN SELECT RAISE(ABORT, 'injected delete failure'); END");
        loaded.clear();
        rotation.singerDelete(2);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(loaded.size(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE singer = 2").toInt(), 2);
        QCOMPARE(value("SELECT COUNT(*) FROM rotationsingers").toInt(), 3);
        exec("DROP TRIGGER fail_singer_delete");
        worker->loadRotation();
        rotation.singerDelete(2);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE singer = 2").toInt(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE singer = 1").toInt(), 1);
    }

    void staleQueueCannotEraseWorkerAppendOrPlayedChange()
    {
        QSignalSpy errors(worker.get(), &DatabaseWorker::errorOccurred);
        QSignalSpy updated(worker.get(), &DatabaseWorker::queueUpdated);
        queue->loadSinger(2);
        deliver(0);
        worker->addSongToQueue(2, 1, 3);
        updated.clear();
        queue->move(1, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(updated.size(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE singer = 2").toInt(), 3);
        QCOMPARE(value("SELECT position FROM queuesongs WHERE qsongid = 21").toInt(), 0);
        queue->loadSinger(2);
        deliver(1);
        worker->setQueueSongPlayed(2, 21, true);
        updated.clear();
        queue->remove(22);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(updated.size(), 0);
        QCOMPARE(value("SELECT played FROM queuesongs WHERE qsongid = 21").toInt(), 1);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs WHERE qsongid = 22").toInt(), 1);
    }

    void queueStatementAndCommitFailuresRollBack()
    {
        QSignalSpy errors(worker.get(), &DatabaseWorker::errorOccurred);
        QSignalSpy updated(worker.get(), &DatabaseWorker::queueUpdated);
        queue->loadSinger(2);
        deliver(0);
        exec("CREATE TRIGGER fail_queue_update BEFORE UPDATE OF position ON queuesongs "
             "WHEN OLD.qsongid = 21 BEGIN SELECT RAISE(ABORT, 'injected queue failure'); END");
        queue->move(1, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(updated.size(), 0);
        QCOMPARE(value("SELECT position FROM queuesongs WHERE qsongid = 22").toInt(), 1);
        QCOMPARE(value("SELECT position FROM queuesongs WHERE qsongid = 21").toInt(), 0);
        exec("DROP TRIGGER fail_queue_update");
        // A deferred foreign key fails at COMMIT after all UPDATEs have succeeded.
        auto background = QSqlDatabase::database("openkj_background");
        QSqlQuery pragma(background);
        QVERIFY(pragma.exec("PRAGMA foreign_keys = ON"));
        exec("CREATE TABLE parent (id INTEGER PRIMARY KEY)");
        exec("CREATE TABLE child (id INTEGER REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)");
        exec("CREATE TRIGGER fail_commit AFTER UPDATE OF position ON queuesongs "
             "BEGIN INSERT INTO child VALUES (999); END");
        queue->loadSinger(2);
        deliver(1);
        queue->move(1, 0);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(updated.size(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM child").toInt(), 0);
        QCOMPARE(value("SELECT position FROM queuesongs WHERE qsongid = 21").toInt(), 0);
        QCOMPARE(value("SELECT position FROM queuesongs WHERE qsongid = 22").toInt(), 1);
        exec("DROP TRIGGER fail_commit");
        queue->loadSinger(2);
        deliver(2);
        queue->move(1, 0);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(updated.size(), 1);
        // Consecutive model commits use the expected result of the previous write.
        queue->move(0, 1);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(updated.size(), 2);
        QCOMPARE(value("SELECT position FROM queuesongs WHERE qsongid = 21").toInt(), 0);
    }

    void compoundWritesAndFailedBeginDoNotReportSuccess()
    {
        QSignalSpy errors(worker.get(), &DatabaseWorker::errorOccurred);
        QSignalSpy added(worker.get(), &DatabaseWorker::singerAdded);
        QSignalSpy updated(worker.get(), &DatabaseWorker::queueUpdated);
        QSignalSpy loaded(worker.get(), &DatabaseWorker::rotationLoaded);
        exec("CREATE TRIGGER fail_queue_insert BEFORE INSERT ON queuesongs "
             "BEGIN SELECT RAISE(ABORT, 'injected insert failure'); END");
        worker->addSingerWithSong("Failed singer", 0, 1, 0);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(added.size(), 0);
        QCOMPARE(updated.size(), 0);
        QCOMPARE(loaded.size(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM rotationsingers").toInt(), 3);
        QVERIFY(library->refreshSong(1));
        queue->loadSinger(2);
        deliver(0);
        QSignalSpy modelErrors(queue.get(), &TableModelQueueSongs::databaseWriteError);
        QCOMPARE(queue->add(1), -1);
        QCOMPARE(modelErrors.size(), 1);
        QCOMPARE(queue->rowCount(), 2);
        exec("DROP TRIGGER fail_queue_insert");
        exec("CREATE TRIGGER fail_rotation_clear BEFORE DELETE ON rotationsingers "
             "BEGIN SELECT RAISE(ABORT, 'injected clear failure'); END");
        worker->clearRotation();
        QCOMPARE(errors.size(), 2);
        QCOMPARE(loaded.size(), 0);
        QCOMPARE(value("SELECT COUNT(*) FROM queuesongs").toInt(), 3);
        auto background = QSqlDatabase::database("openkj_background");
        QVERIFY(background.transaction());
        worker->addSinger("Failed begin", 0, false);
        QCOMPARE(errors.size(), 3);
        QCOMPARE(added.size(), 0);
        QCOMPARE(loaded.size(), 0);
        QVERIFY(background.rollback());
    }

};

QTEST_MAIN(QueueLoadingTest)
#include "queue_loading_test.moc"
