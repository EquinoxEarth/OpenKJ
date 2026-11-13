#include "../src/databaseschema.h"
#include "../src/databasecleanup.h"
#include "../src/trackgaincache.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

class DatabaseSchemaTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_directory;
    QSqlDatabase m_database;
    QString m_path;

    void exec(const QString &sql)
    {
        QSqlQuery query(m_database);
        QVERIFY2(query.exec(sql), qPrintable(query.lastError().text()));
    }

    QVariant value(const QString &sql)
    {
        QSqlQuery query(m_database);
        if (!query.exec(sql) || !query.next()) {
            QTest::qFail(qPrintable(sql + " " + query.lastError().text()), __FILE__, __LINE__);
            return {};
        }
        return query.value(0);
    }

    void createLegacyDatabase()
    {
        m_database.setDatabaseName(m_path);
        QVERIFY2(m_database.open(), qPrintable(m_database.lastError().text()));
        exec("CREATE TABLE dbsongs (songid INTEGER PRIMARY KEY, artist COLLATE NOCASE, title COLLATE NOCASE, discid COLLATE NOCASE, duration INTEGER, path TEXT UNIQUE, filename TEXT)");
        exec("CREATE TABLE sourceDirs (path TEXT UNIQUE, pattern INTEGER, custompattern INTEGER)");
        exec("CREATE TABLE custompatterns (patternid INTEGER PRIMARY KEY, name TEXT, artistregex TEXT, artistcapturegrp INT, titleregex TEXT, titlecapturegrp INT, discidregex TEXT, discidcapturegrp INT)");
        exec("CREATE TABLE regularSingers (regsingerid INTEGER PRIMARY KEY, name COLLATE NOCASE UNIQUE)");
        exec("CREATE TABLE regularSongs (regsongid INTEGER PRIMARY KEY, regsingerid INTEGER NOT NULL, songid INTEGER NOT NULL, keychg INTEGER, position INTEGER)");
        exec("CREATE UNIQUE INDEX idx_path ON dbsongs(path)");
        exec("PRAGMA user_version = 102");
        exec("INSERT INTO dbsongs VALUES (1, 'Artist', 'Title', 'DISC-1', 0, '/offline/song.zip', 'song.zip'),"
             "(2, 'Artist', 'Second song', 'DISC-2', 0, '/offline/second.zip', 'second.zip'),"
             "(3, '--Dropped Song--', 'External', '!!DROPPED!!', 0, '/offline/drop.zip', 'drop.zip')");
        exec("INSERT INTO regularSingers VALUES (1, 'O''Brien'), (2, 'Empty'), (3, 'Dropped only')");
        exec("INSERT INTO regularSongs VALUES (1, 1, 1, -4, 3), (2, 1, 1, 2, 1),"
             "(3, 1, 2, NULL, 2), (4, 1, 999, 0, 4), (5, 3, 3, 0, 1)");
    }

    void migrate()
    {
        m_database.close();
        const auto error = okj::openAndMigrateDatabase(m_database, m_path);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

private slots:
    void init()
    {
        QVERIFY(m_directory.isValid());
        m_path = m_directory.filePath(QString::fromLatin1(QTest::currentTestFunction()) + "/openkj.sqlite");
        QVERIFY(QDir().mkpath(QFileInfo(m_path).absolutePath()));
        m_database = QSqlDatabase::addDatabase("QSQLITE", "database-schema-test");
    }

    void cleanup()
    {
        m_database.close();
        m_database = {};
        QSqlDatabase::removeDatabase("database-schema-test");
    }

    void clearKaraokeLibraryRollsBackAndKeepsHistory()
    {
        migrate();
        const auto trackPath = QFileInfo(m_path).absoluteDir().filePath("track.zip");
        {
            QFile track(trackPath);
            QVERIFY(track.open(QIODevice::WriteOnly));
            QCOMPARE(track.write("track"), 5);
        }
        const auto key = okj::TrackGainKey::fromFile(trackPath);
        const auto cachePath = QFileInfo(m_path).absoluteDir().filePath("audio-normalization.sqlite");
        okj::cacheTrackGain(m_path, key, 0.5);
        QVERIFY(QFileInfo::exists(cachePath));
        QCOMPARE(okj::cachedTrackGain(m_path, key).value_or(-1.0), 0.5);
        const auto generation = okj::trackGainCacheGeneration();
        exec("INSERT INTO dbSongs (songid, path) VALUES (1, '/music/song.zip')");
        exec("INSERT INTO regularSingers (regsingerid, name) VALUES (1, 'Singer')");
        exec("INSERT INTO regularSongs (regsingerid, songid) VALUES (1, 1)");
        exec("INSERT INTO rotationSingers (singerid, name, position, regularid) VALUES (1, 'Singer', 0, 1)");
        exec("INSERT INTO queueSongs (singer, song) VALUES (1, 1)");
        exec("INSERT INTO historySingers (id, name) VALUES (1, 'Singer')");
        exec("INSERT INTO historySongs (historySinger, filepath) VALUES (1, '/music/song.zip')");
        exec("INSERT INTO sourceDirs (path) VALUES ('/music')");
        exec("INSERT INTO bmplaylists (title) VALUES ('Break')");
        exec("CREATE TRIGGER block_clear BEFORE DELETE ON dbSongs "
             "BEGIN SELECT RAISE(ABORT, 'clear blocked'); END");

        const QStringList cleared{"dbSongs", "regularSingers", "regularSongs", "rotationSingers", "queueSongs"};
        const auto error = okj::clearKaraokeLibrary(m_database);
        QVERIFY2(error.contains("clear blocked"), qPrintable(error));
        QVERIFY(QFileInfo::exists(cachePath));
        QCOMPARE(okj::trackGainCacheGeneration(), generation);
        for (const auto &table : cleared)
            QCOMPARE(value("SELECT COUNT(*) FROM " + table).toInt(), 1);

        exec("DROP TRIGGER block_clear");
        const auto retry = okj::clearKaraokeLibrary(m_database);
        QVERIFY2(retry.isEmpty(), qPrintable(retry));
        QVERIFY(!QFileInfo::exists(cachePath));
        QVERIFY(!okj::cachedTrackGain(m_path, key));
        QVERIFY(okj::trackGainCacheGeneration() != generation);
        for (const auto &table : cleared)
            QCOMPARE(value("SELECT COUNT(*) FROM " + table).toInt(), 0);
        for (const auto &table : {"historySingers", "historySongs", "sourceDirs", "bmplaylists"})
            QCOMPARE(value(QString("SELECT COUNT(*) FROM ") + table).toInt(), 1);
    }

    void deleteBreakPlaylistRollsBackWhenDefaultCannotBeCreated()
    {
        migrate();
        exec("INSERT INTO bmplaylists (playlistid, title) VALUES (1, 'First'), (2, 'Last')");
        exec("INSERT INTO bmplsongs (playlist, path) VALUES (1, '/first.mp3'), (2, '/last.mp3')");

        const auto first = okj::deleteBreakPlaylist(m_database, 1);
        QVERIFY2(first.isEmpty(), qPrintable(first));
        QCOMPARE(value("SELECT title FROM bmplaylists").toString(), QString("Last"));
        QCOMPARE(value("SELECT path FROM bmplsongs").toString(), QString("/last.mp3"));

        exec("CREATE TRIGGER block_default BEFORE INSERT ON bmplaylists "
             "BEGIN SELECT RAISE(ABORT, 'default blocked'); END");
        const auto error = okj::deleteBreakPlaylist(m_database, 2);
        QVERIFY2(error.contains("default blocked"), qPrintable(error));
        QCOMPARE(value("SELECT title FROM bmplaylists").toString(), QString("Last"));
        QCOMPARE(value("SELECT path FROM bmplsongs").toString(), QString("/last.mp3"));

        exec("DROP TRIGGER block_default");
        const auto retry = okj::deleteBreakPlaylist(m_database, 2);
        QVERIFY2(retry.isEmpty(), qPrintable(retry));
        QCOMPARE(value("SELECT title FROM bmplaylists").toString(), QString("Default"));
        QCOMPARE(value("SELECT COUNT(*) FROM bmplsongs").toInt(), 0);
        QVERIFY(!okj::deleteBreakPlaylist(m_database, 2).isEmpty());
        QCOMPARE(value("SELECT title FROM bmplaylists").toString(), QString("Default"));
    }

    void freshDatabase()
    {
        migrate();
        QCOMPARE(value("PRAGMA user_version").toInt(), 106);
        QCOMPARE(value("PRAGMA synchronous").toInt(), 2);
        exec("INSERT INTO dbsongs (path, searchstring) VALUES ('/library/song.zip', 'search')");
        QCOMPARE(value("SELECT plays FROM dbsongs").toInt(), 0);
        QCOMPARE(value("SELECT count(*) FROM historySongs").toInt(), 0);
        migrate();
        QCOMPARE(value("SELECT count(*) FROM dbsongs").toInt(), 1);
    }

    void legacyHistoryIsImportedOnce()
    {
        createLegacyDatabase();
        migrate();
        QCOMPARE(value("PRAGMA user_version").toInt(), 106);
        QCOMPARE(value("SELECT searchstring FROM dbsongs WHERE songid = 1").toString(),
                 QString("song.zip Artist Title DISC-1"));
        QCOMPARE(value("SELECT count(*) FROM historySingers").toInt(), 1);
        QCOMPARE(value("SELECT name FROM historySingers").toString(), QString("O'Brien"));
        QCOMPARE(value("SELECT count(*) FROM historySongs").toInt(), 2);
        QCOMPARE(value("SELECT plays FROM historySongs WHERE songid = 'DISC-1'").toInt(), 2);
        QCOMPARE(value("SELECT keychange FROM historySongs WHERE songid = 'DISC-1'").toInt(), -4);
        QCOMPARE(value("SELECT keychange FROM historySongs WHERE songid = 'DISC-2'").toInt(), 0);
        QVERIFY(value("SELECT lastplay FROM historySongs WHERE songid = 'DISC-1'").toDateTime().isValid());
        migrate();
        QCOMPARE(value("SELECT count(*) FROM historySongs").toInt(), 2);
        QCOMPARE(value("SELECT plays FROM historySongs WHERE songid = 'DISC-1'").toInt(), 2);
        exec("DELETE FROM historySongs WHERE songid = 'DISC-2'");
        migrate();
        QCOMPARE(value("SELECT count(*) FROM historySongs").toInt(), 1);
    }

    void failedImportRollsBackAndCanRetry()
    {
        createLegacyDatabase();
        // Simulate a partially prepared schema left by a previous release.
        exec("CREATE TABLE historySongs (id INTEGER PRIMARY KEY, historySinger INT NOT NULL, filepath TEXT NOT NULL, "
             "artist TEXT, title TEXT, songid TEXT, keychange INT, plays INT, lastplay TIMESTAMP)");
        exec("CREATE TRIGGER fail_history BEFORE INSERT ON historySongs "
             "WHEN NEW.filepath = '/offline/second.zip' BEGIN SELECT RAISE(ABORT, 'injected history failure'); END");
        m_database.close();
        const auto error = okj::openAndMigrateDatabase(m_database, m_path);
        QVERIFY2(error.contains("injected history failure"), qPrintable(error));
        m_database.close();
        QVERIFY(m_database.open());
        QCOMPARE(value("PRAGMA user_version").toInt(), 102);
        QCOMPARE(value("SELECT count(*) FROM historySongs").toInt(), 0);
        QCOMPARE(value("SELECT count(*) FROM sqlite_master WHERE name IN ('historySingers', 'dbSongHistory')").toInt(), 0);
        QCOMPARE(value("SELECT count(*) FROM pragma_table_info('dbsongs') WHERE name IN ('searchstring', 'plays', 'lastplay')").toInt(), 0);
        QCOMPARE(value("SELECT count(*) FROM regularSongs").toInt(), 5);

        exec("DROP TRIGGER fail_history");
        migrate();
        QCOMPARE(value("PRAGMA user_version").toInt(), 106);
        QCOMPARE(value("SELECT count(*) FROM historySongs").toInt(), 2);
        QCOMPARE(value("SELECT plays FROM historySongs WHERE songid = 'DISC-1'").toInt(), 2);
    }

    void existingHistoryIsPreserved()
    {
        createLegacyDatabase();
        migrate();
        exec("UPDATE historySongs SET plays = 12, keychange = 5 WHERE songid = 'DISC-1'");
        exec("DELETE FROM historySongs WHERE songid = 'DISC-2'");
        exec("PRAGMA user_version = 105");
        migrate();
        QCOMPARE(value("PRAGMA user_version").toInt(), 106);
        QCOMPARE(value("SELECT count(*) FROM historySingers").toInt(), 1);
        QCOMPARE(value("SELECT count(*) FROM historySongs").toInt(), 2);
        QCOMPARE(value("SELECT plays FROM historySongs WHERE songid = 'DISC-1'").toInt(), 12);
        QCOMPARE(value("SELECT keychange FROM historySongs WHERE songid = 'DISC-1'").toInt(), 5);
    }

    void databaseCannotBeOpened()
    {
        QVERIFY(QDir().mkpath(m_path));
        const auto error = okj::openAndMigrateDatabase(m_database, m_path);
        QVERIFY(!error.isEmpty());
        QVERIFY(!m_database.isOpen());
    }

    void incompleteCurrentSchemaIsReported()
    {
        migrate();
        exec("DROP TABLE historySongs");
        m_database.close();
        const auto error = okj::openAndMigrateDatabase(m_database, m_path);
        QVERIFY2(error.contains("historySongs"), qPrintable(error));
        QCOMPARE(value("PRAGMA user_version").toInt(), 106);
    }
};

QTEST_GUILESS_MAIN(DatabaseSchemaTest)
#include "database_schema_test.moc"
