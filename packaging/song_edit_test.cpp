#include "../src/databaseschema.h"
#include "../src/songedit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class SongEditTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_root;
    QDir m_folder;
    QSqlDatabase m_database;
    const okj::SongEdit m_edit{"New Artist", "New Title", "NEW-1", "New Track"};

    QString path(const QString &name) const
    {
        return m_folder.filePath(name);
    }

    static bool writeFile(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(contents) == contents.size();
    }

    static QByteArray contents(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static QString rename(const QString &source, const QString &destination)
    {
        QFile file(source);
        return file.rename(destination) ? QString() : file.errorString();
    }

    void exec(const QString &sql)
    {
        QSqlQuery query(m_database);
        QVERIFY2(query.exec(sql), qPrintable(query.lastError().text()));
    }

    void addSong(const QString &oldPath)
    {
        QSqlQuery query(m_database);
        QVERIFY(query.prepare("INSERT INTO dbsongs (songid, artist, title, discid, path, filename, searchstring) "
                              "VALUES (42, 'Artist', 'Title', 'DISC-1', ?, 'original filename field', 'original search')"));
        query.addBindValue(oldPath);
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
        QVERIFY(query.prepare("INSERT INTO historySongs (historySinger, filepath, plays, keychange) VALUES (1, ?, 9, -3)"));
        query.addBindValue(oldPath);
        QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
    }

    void verifyRecord(const QString &savedPath, const QString &title, const QString &filename)
    {
        QSqlQuery query(m_database);
        QVERIFY(query.exec("SELECT path, title, filename FROM dbsongs WHERE songid = 42"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), savedPath);
        QCOMPARE(query.value(1).toString(), title);
        QCOMPARE(query.value(2).toString(), filename);
        QVERIFY(query.exec("SELECT filepath, plays, keychange FROM historySongs WHERE historySinger = 1"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), savedPath);
        QCOMPARE(query.value(1).toInt(), 9);
        QCOMPARE(query.value(2).toInt(), -3);
    }

    void addPair()
    {
        QVERIFY(writeFile(path("Old Track.CDG"), "lyrics"));
        QVERIFY(writeFile(path("Old Track.MP3"), "audio"));
        addSong(path("Old Track.CDG"));
    }

    void verifyOriginalPair()
    {
        QCOMPARE(contents(path("Old Track.CDG")), QByteArray("lyrics"));
        QCOMPARE(contents(path("Old Track.MP3")), QByteArray("audio"));
        QVERIFY(!QFile::exists(path("New Track.CDG")));
        verifyRecord(path("Old Track.CDG"), "Title", "original filename field");
    }

private slots:
    void init()
    {
        QVERIFY(m_root.isValid());
        m_folder = QDir(m_root.filePath(QTest::currentTestFunction()));
        QVERIFY(QDir().mkpath(m_folder.absolutePath()));
        m_database = QSqlDatabase::addDatabase("QSQLITE", "song-edit-test");
        const auto error = okj::openAndMigrateDatabase(m_database, path("openkj.sqlite"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void cleanup()
    {
        m_database.close();
        m_database = {};
        QSqlDatabase::removeDatabase("song-edit-test");
    }

    void renamesSingleFileAndPreservesHistory()
    {
        const auto oldPath = path("Old Track.zip");
        QVERIFY(writeFile(oldPath, "song"));
        addSong(oldPath);
        const auto error = okj::saveSongEdit(m_database, 42, oldPath, m_edit);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFile::exists(oldPath));
        QCOMPARE(contents(path("New Track.zip")), QByteArray("song"));
        verifyRecord(path("New Track.zip"), "New Title", "New Track.zip");
    }

    void renamesPairAndPreservesExtensions()
    {
        addPair();
        const auto error = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), m_edit);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFile::exists(path("Old Track.CDG")));
        QVERIFY(!QFile::exists(path("Old Track.MP3")));
        QCOMPARE(contents(path("New Track.CDG")), QByteArray("lyrics"));
        QCOMPARE(contents(path("New Track.MP3")), QByteArray("audio"));
        verifyRecord(path("New Track.CDG"), "New Title", "New Track.CDG");
    }

    void supportsCaseOnlyRename()
    {
        addPair();
        auto edit = m_edit;
        edit.filenameBase = "OLD TRACK";
        const auto error = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), edit);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto files = m_folder.entryList(QDir::Files);
        QVERIFY(files.contains("OLD TRACK.CDG") && files.contains("OLD TRACK.MP3"));
        verifyRecord(path("OLD TRACK.CDG"), "New Title", "OLD TRACK.CDG");
    }

    void metadataOnlyWorksWithUnavailableMedia()
    {
        const auto oldPath = path("Unavailable Track.zip");
        addSong(oldPath);
        const okj::SongEdit edit{"New Artist", "New Title", "NEW-1", {}};
        const auto error = okj::saveSongEdit(m_database, 42, oldPath, edit);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        verifyRecord(oldPath, "New Title", "original filename field");
    }

    void rejectsExistingAudioDestinationBeforeMoving()
    {
        addPair();
        QVERIFY(writeFile(path("New Track.MP3"), "unrelated audio"));
        const auto error = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), m_edit,
                                           [](const QString &, const QString &) { return QString("Unexpected move"); });
        QVERIFY2(error.contains("already exists") && error.contains(path("New Track.MP3")), qPrintable(error));
        verifyOriginalPair();
        QCOMPARE(contents(path("New Track.MP3")), QByteArray("unrelated audio"));
    }

    void rejectsStaleLibraryPath()
    {
        addPair();
        exec("UPDATE dbsongs SET path = '/library/moved.cdg' WHERE songid = 42");
        const auto error = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), m_edit);
        QVERIFY2(error.contains("entry changed"), qPrintable(error));
        QCOMPARE(contents(path("Old Track.CDG")), QByteArray("lyrics"));
        QCOMPARE(contents(path("Old Track.MP3")), QByteArray("audio"));
        QVERIFY(!QFile::exists(path("New Track.CDG")));
    }

    void restoresFirstMoveWhenSecondFails()
    {
        addPair();
        const auto error = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), m_edit,
            [](const QString &source, const QString &destination) {
                if (source.endsWith("Old Track.MP3") && !writeFile(destination, "late collision"))
                    return QString("Could not create the test collision");
                return rename(source, destination);
            });
        QVERIFY2(error.contains("Could not rename the file") && error.contains(path("New Track.MP3")), qPrintable(error));
        verifyOriginalPair();
        QCOMPARE(contents(path("New Track.MP3")), QByteArray("late collision"));
    }

    void restoresPairWhenAnotherAudioFileWouldPlay()
    {
        const auto oldPath = path("Old Track.CDG");
        QVERIFY(writeFile(oldPath, "lyrics"));
        QVERIFY(writeFile(path("Old Track.WAV"), "audio"));
        QVERIFY(writeFile(path("New Track.MP3"), "unrelated audio"));
        addSong(oldPath);
        const auto error = okj::saveSongEdit(m_database, 42, oldPath, m_edit);
        QVERIFY2(error.contains("different audio file"), qPrintable(error));
        QCOMPARE(contents(oldPath), QByteArray("lyrics"));
        QCOMPARE(contents(path("Old Track.WAV")), QByteArray("audio"));
        QCOMPARE(contents(path("New Track.MP3")), QByteArray("unrelated audio"));
        QVERIFY(!QFile::exists(path("New Track.CDG")));
        QVERIFY(!QFile::exists(path("New Track.WAV")));
        verifyRecord(oldPath, "Title", "original filename field");
    }

    void reportsPathsWhenRestorationFails()
    {
        addPair();
        const auto error = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), m_edit,
            [](const QString &source, const QString &destination) {
                if (source.endsWith("Old Track.MP3")) {
                    const auto previousCdg = QFileInfo(source).dir().filePath("Old Track.CDG");
                    if (!writeFile(previousCdg, "unrelated file at original location"))
                        return QString("Could not create the test collision");
                    return QString("injected audio move failure");
                }
                return rename(source, destination);
            });
        QVERIFY2(error.contains("Could not restore the original file") && error.contains(path("New Track.CDG")) &&
                 error.contains(path("Old Track.CDG")), qPrintable(error));
        QCOMPARE(contents(path("New Track.CDG")), QByteArray("lyrics"));
        QCOMPARE(contents(path("Old Track.CDG")), QByteArray("unrelated file at original location"));
        QCOMPARE(contents(path("Old Track.MP3")), QByteArray("audio"));
        verifyRecord(path("Old Track.CDG"), "Title", "original filename field");
    }

    void removesOnlyPlaybackCompanionAndPreservesHistory()
    {
        const auto cdg = path("Pair.cdg");
        const auto wav = path("Pair.wav");
        const auto ogg = path("Pair.ogg");
        QVERIFY(writeFile(cdg, "lyrics"));
        QVERIFY(writeFile(wav, "playback audio"));
        QVERIFY(writeFile(ogg, "alternate audio"));
        addSong(cdg);
        exec("INSERT INTO queueSongs (singer, song) VALUES (1, 42)");
        exec("INSERT INTO regularSongs (regsingerid, songid) VALUES (1, 42)");
        const auto paths = okj::songRemovalPaths(cdg);
        QCOMPARE(paths, QStringList({cdg, wav}));
        const auto failedMove = okj::removeSongFiles(m_database, 42, cdg, paths,
            [](const QString &source, const QString &destination) {
                return source.endsWith("/Pair.wav") ? QString("injected audio move failure") : rename(source, destination);
            });
        QVERIFY(failedMove.error.contains("injected audio move failure"));
        QCOMPARE(contents(cdg), QByteArray("lyrics"));
        QCOMPARE(contents(wav), QByteArray("playback audio"));
        verifyRecord(cdg, "Title", "original filename field");

        exec("PRAGMA foreign_keys = ON");
        exec("CREATE TABLE removalParent (id INTEGER PRIMARY KEY)");
        exec("CREATE TABLE removalChild (parentId INT REFERENCES removalParent(id) DEFERRABLE INITIALLY DEFERRED)");
        exec("CREATE TRIGGER fail_remove AFTER DELETE ON dbsongs BEGIN INSERT INTO removalChild VALUES (99); END");
        const auto failedCommit = okj::removeSongFiles(m_database, 42, cdg, paths);
        QVERIFY2(failedCommit.error.contains("FOREIGN KEY", Qt::CaseInsensitive), qPrintable(failedCommit.error));
        QCOMPARE(contents(cdg), QByteArray("lyrics"));
        QCOMPARE(contents(wav), QByteArray("playback audio"));
        verifyRecord(cdg, "Title", "original filename field");
        QSqlQuery query(m_database);
        QVERIFY(query.exec("SELECT (SELECT COUNT(*) FROM queueSongs), (SELECT COUNT(*) FROM regularSongs)"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        QCOMPARE(query.value(1).toInt(), 1);
        query.finish();

        exec("DROP TRIGGER fail_remove");
        const auto result = okj::removeSongFiles(m_database, 42, cdg, paths);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!QFile::exists(cdg));
        QVERIFY(!QFile::exists(wav));
        QCOMPARE(contents(ogg), QByteArray("alternate audio"));
        QVERIFY(query.exec("SELECT (SELECT COUNT(*) FROM dbSongs), (SELECT COUNT(*) FROM queueSongs), "
                           "(SELECT COUNT(*) FROM regularSongs), (SELECT plays FROM historySongs)"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        QCOMPARE(query.value(1).toInt(), 0);
        QCOMPARE(query.value(2).toInt(), 0);
        QCOMPARE(query.value(3).toInt(), 9);
    }

    void rollsBackPairOnDatabaseCommitFailure()
    {
        addPair();
        exec("PRAGMA foreign_keys = ON");
        exec("CREATE TABLE editParent (id INTEGER PRIMARY KEY)");
        exec("CREATE TABLE editChild (parentId INT REFERENCES editParent(id) DEFERRABLE INITIALLY DEFERRED)");
        exec("CREATE TRIGGER fail_edit AFTER UPDATE OF path ON dbsongs BEGIN INSERT INTO editChild VALUES (99); END");
        const auto error = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), m_edit);
        QVERIFY2(error.contains("FOREIGN KEY", Qt::CaseInsensitive), qPrintable(error));
        verifyOriginalPair();
        QVERIFY(!QFile::exists(path("New Track.MP3")));
        exec("DROP TRIGGER fail_edit");
        const auto retry = okj::saveSongEdit(m_database, 42, path("Old Track.CDG"), m_edit);
        QVERIFY2(retry.isEmpty(), qPrintable(retry));
        verifyRecord(path("New Track.CDG"), "New Title", "New Track.CDG");
    }
};

QTEST_GUILESS_MAIN(SongEditTest)
#include "song_edit_test.moc"
