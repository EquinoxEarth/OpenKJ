// Uses an isolated in-memory database; no host library or media is opened.
#include "models/tablemodelbreaksongs.h"
#include "models/tablemodelplaylistsongs.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTest>
#include <spdlog/sinks/null_sink.h>
#include <algorithm>
#include <stdexcept>

namespace {
void exec(const QString &sql)
{
    QSqlQuery query;
    if (!query.exec(sql))
        throw std::runtime_error(query.lastError().text().toStdString());
}
const QString externalPath = QStringLiteral("/Disconnected/Music/Björk – 夜.mp3");
}

class BreakSongLookupsTest : public QObject {
    Q_OBJECT
    QSqlDatabase database;
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        spdlog::null_logger_mt("logger");
        database = QSqlDatabase::addDatabase("QSQLITE");
        database.setDatabaseName(":memory:");
        QVERIFY(database.open());
        exec("CREATE TABLE bmsongs (songid INTEGER PRIMARY KEY, artist TEXT, title TEXT, "
             "path TEXT NOT NULL UNIQUE, filename TEXT, duration INT, searchstring TEXT)");
        exec("CREATE TABLE bmplsongs (plsongid INTEGER PRIMARY KEY, playlist INT, position INT, "
             "artist INT, title INT, filename INT, duration INT, path INT)");
    }

    void init()
    {
        exec("DELETE FROM bmplsongs");
        exec("DELETE FROM bmsongs");
        exec("INSERT INTO bmsongs VALUES "
             "(101,'Zulu','Last','/Disconnected/Music/Björk – 夜.mp3','夜.mp3',180,'zulu last'),"
             "(202,'Alpha','First','/Music/Case.mp3','Case.mp3',120,'alpha first'),"
             "(303,'Middle','Second','/Music/case.mp3','case.mp3',240,'middle second')");
    }

    void lookupsSurviveFilteringAndSorting()
    {
        TableModelBreakSongs library;
        library.loadDatabase();
        auto *original = &library.getSong(101);
        library.search("alpha");
        QCOMPARE(library.rowCount(), 1);
        QCOMPARE(library.getSongId(externalPath), 101);
        QCOMPARE(library.getSong(101).title, QString("Last"));
        QCOMPARE(library.getSongId("/Music/Case.mp3"), 202);
        QCOMPARE(library.getSongId("/Music/case.mp3"), 303);
        QCOMPARE(library.getSongId("/Music/CASE.mp3"), -1);
        QCOMPARE(library.getSongId("/missing.mp3"), -1);
        QVERIFY_EXCEPTION_THROWN(library.getSong(-1), std::out_of_range);
        library.search("");
        for (int column = 0; column < library.columnCount(); ++column) {
            for (const auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                library.sort(column, order);
                QCOMPARE(&library.getSong(101), original);
                QCOMPARE(library.getSong(202).path, QString("/Music/Case.mp3"));
                QCOMPARE(library.getSongId(externalPath), 101);
            }
        }
    }

    void reloadRebuildsIndexes()
    {
        TableModelBreakSongs library;
        library.loadDatabase();
        library.search("alpha");
        exec("DELETE FROM bmsongs WHERE songid=202");
        exec("UPDATE bmsongs SET path='/Moved/song.mp3' WHERE songid=101");
        exec("INSERT INTO bmsongs VALUES (404,'Alpha','New','/New/song.mp3','song.mp3',99,'alpha new')");
        library.loadDatabase();
        QCOMPARE(library.rowCount(), 1);
        QCOMPARE(library.index(0, TableModelBreakSongs::COL_ID).data().toInt(), 404);
        QCOMPARE(library.getSongId(externalPath), -1);
        QCOMPARE(library.getSongId("/Moved/song.mp3"), 101);
        QCOMPARE(library.getSongId("/Music/Case.mp3"), -1);
        QCOMPARE(library.getSongId("/New/song.mp3"), 404);
        QCOMPARE(library.getSong(101).path, QString("/Moved/song.mp3"));
        QVERIFY_EXCEPTION_THROWN(library.getSong(202), std::out_of_range);
        exec("DELETE FROM bmsongs");
        library.loadDatabase();
        QCOMPARE(library.rowCount(), 0);
        QCOMPARE(library.getSongId("/Moved/song.mp3"), -1);
        QVERIFY_EXCEPTION_THROWN(library.getSong(101), std::out_of_range);
        exec("INSERT INTO bmsongs VALUES (202,'Reused','ID','/Reused/song.mp3','song.mp3',88,'reused id')");
        library.loadDatabase();
        QCOMPARE(library.getSongId("/Music/Case.mp3"), -1);
        QCOMPARE(library.getSongId("/Reused/song.mp3"), 202);
        QCOMPARE(library.getSong(202).duration, 88);
    }

    void playlistOrderAndMetadata()
    {
        TableModelBreakSongs library;
        library.loadDatabase();
        library.search("no matches");
        QCOMPARE(library.rowCount(), 0);
        TableModelPlaylistSongs playlist(library);
        playlist.setCurrentPlaylist(42);
        const QList<QString> paths{"/Music/case.mp3", externalPath, "/Music/Case.mp3", externalPath};
        const QList<int> expected{303,101,202,101};
        for (const auto &path : paths) {
            const int id = library.getSongId(path);
            QVERIFY(id >= 0);
            playlist.addSong(id);
        }
        playlist.savePlaylistChanges();
        playlist.setCurrentPlaylist(42);
        QCOMPARE(playlist.rowCount(), paths.size());
        for (int position = 0; position < paths.size(); ++position) {
            const auto entry = playlist.getPlSongByPosition(position);
            QVERIFY(entry.has_value());
            const auto &song = entry->get();
            QCOMPARE(song.breakSongId, expected[position]);
            QCOMPARE(song.path, paths[position]);
            QCOMPARE(song.position, position);
            QCOMPARE(song.artist, library.getSong(expected[position]).artist);
            QCOMPARE(song.title, library.getSong(expected[position]).title);
            QCOMPARE(song.duration, library.getSong(expected[position]).duration);
        }
    }

    void largeLibraryLookupComparison()
    {
        exec("DELETE FROM bmsongs");
        exec("WITH RECURSIVE n(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM n WHERE id<100000) "
             "INSERT INTO bmsongs SELECT id, printf('Artist %06d',id), printf('Title %06d',id), "
             "printf('/Music/%d.mp3',id), printf('%d.mp3',id), id*2, 'artist title' FROM n");
        TableModelBreakSongs library;
        library.loadDatabase();
        std::vector<BreakSong> reference;
        reference.reserve(100000);
        for (int id = 1; id <= 100000; ++id)
            reference.push_back(library.getSong(id));
        QList<QPair<QString, int>> requests;
        qint64 expected = 0;
        for (int request = 0; request < 2000; ++request) {
            const int id = (request * 7919) % 100000 + 1;
            const bool missing = request % 3 == 0;
            requests.append({missing ? QString("/Missing/%1.mp3").arg(id)
                                     : QString("/Music/%1.mp3").arg(id), missing ? -1 : id});
            expected += missing ? -1 : id * 3;
        }
        QElapsedTimer timer;
        timer.start();
        qint64 indexed = 0;
        for (const auto &request : requests) {
            const int id = library.getSongId(request.first);
            indexed += id;
            if (id >= 0)
                indexed += library.getSong(id).duration;
        }
        const auto indexedNs = timer.nsecsElapsed();
        timer.restart();
        qint64 scanned = 0;
        // Same two linear searches as the previous path/ID lookup implementation.
        for (const auto &request : requests) {
            const auto path = std::find_if(reference.begin(), reference.end(), [&](const auto &song) {
                return song.path == request.first;
            });
            const int id = path == reference.end() ? -1 : path->id;
            scanned += id;
            if (id >= 0) {
                const auto song = std::find_if(reference.begin(), reference.end(), [&](const auto &item) {
                    return item.id == id;
                });
                scanned += song->duration;
            }
        }
        QCOMPARE(indexed, expected);
        QCOMPARE(scanned, expected);
        qInfo("100,000 songs, 2,000 requests: indexed %.3f ms; linear scans %.3f ms",
              indexedNs / 1000000.0, timer.nsecsElapsed() / 1000000.0);
    }

    void cleanupTestCase()
    {
        database.close();
        database = {};
        QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    }
};

QTEST_MAIN(BreakSongLookupsTest)
#include "break_song_lookups_test.moc"
