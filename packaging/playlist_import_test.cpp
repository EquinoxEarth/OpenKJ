// Every file and database belongs to a temporary test directory.
#include "breakplaylistimporter.h"

#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
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

class PlaylistImportTest : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QSqlDatabase database;
    int oldThreads = 0;
    QString relative, absolute;

    BreakPlaylistImportRequest request() const
    {
        return {database.databaseName(), temporary.filePath("input.m3u"), QStringLiteral("Party \"夜\"")};
    }
    void writePlaylist(const QStringList &entries)
    {
        QFile file(request().filename);
        if (!file.open(QIODevice::WriteOnly))
            throw std::runtime_error(file.errorString().toStdString());
        file.write(QByteArray::fromHex("efbbbf"));
        file.write((QString("#EXTM3U\r\n") + entries.join("\r\n") + "\r\n").toUtf8());
    }
    void writeWave(const QString &path)
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RIFF", 4);
        stream << quint32(32036);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000)
               << quint16(2) << quint16(16);
        stream.writeRawData("data", 4);
        stream << quint32(32000);
        data.append(QByteArray(32000, '\0'));
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
                throw std::runtime_error("Cannot create WAV fixture");
        }
#ifdef Q_OS_WIN
        const auto filename = path.toStdWString();
        TagLib::FileRef audio(filename.c_str());
#else
        const auto filename = QFile::encodeName(path);
        TagLib::FileRef audio(filename.constData());
#endif
        if (audio.isNull() || !audio.tag())
            throw std::runtime_error("Cannot tag WAV fixture");
        audio.tag()->setArtist(TagLib::String("O'Connor & \"Night\" 夜", TagLib::String::UTF8));
        audio.tag()->setTitle(TagLib::String("Don't \"Stop\"", TagLib::String::UTF8));
        if (!audio.save())
            throw std::runtime_error("Cannot save WAV tags");
    }
    void addKnown()
    {
        exec("INSERT INTO bmsongs VALUES (101,'Preserved','Offline','/Disconnected/known.mp3','known.mp3',123,'preserved offline')");
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
        exec("CREATE TABLE bmplaylists (playlistid INTEGER PRIMARY KEY, title TEXT COLLATE NOCASE UNIQUE)");
        exec("CREATE TABLE bmplsongs (plsongid INTEGER PRIMARY KEY, playlist INT, position INT, "
             "artist INT, title INT, filename INT, duration INT, path INT)");
        QVERIFY(QDir(temporary.path()).mkpath("nested"));
        relative = QStringLiteral("nested/Zoë 夜.wav");
        absolute = temporary.filePath("absolute.wav");
        writeWave(temporary.filePath(relative));
        writeWave(absolute);
        oldThreads = QThreadPool::globalInstance()->maxThreadCount();
        QThreadPool::globalInstance()->setMaxThreadCount(1);
    }
    void init()
    {
        exec("DROP TRIGGER IF EXISTS reject_import");
        exec("DELETE FROM bmplsongs");
        exec("DELETE FROM bmplaylists");
        exec("DELETE FROM bmsongs");
    }

    void pathsTagsAndOrder()
    {
        addKnown();
        auto windowsRelative = relative;
        windowsRelative.replace('/', '\\');
        writePlaylist({"#EXTINF:2,Song", windowsRelative, "", absolute,
                       "missing.mp3", windowsRelative, "/Disconnected/known.mp3"});
        BreakPlaylistImporter importer;
        bool wrongThread = false;
        connect(&importer, &BreakPlaylistImporter::finished, &importer, [&](const auto &) {
            wrongThread = QThread::currentThread() != qApp->thread();
        }, Qt::DirectConnection);
        QSignalSpy finished(&importer, &BreakPlaylistImporter::finished);
        QVERIFY(importer.start(request()));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished[0][0].value<BreakPlaylistImportResult>();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!result.cancelled && !importer.isRunning() && !wrongThread);
        QCOMPARE(result.imported, 4);
        QCOMPARE(result.skipped, 1);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 3);
        QCOMPARE(scalar("SELECT title FROM bmplaylists").toString(), request().playlistTitle);
        QSqlQuery songs("SELECT s.path,s.artist,s.title,s.duration FROM bmplsongs p "
                        "JOIN bmsongs s ON s.songid=p.artist ORDER BY p.position");
        const QStringList paths{temporary.filePath(relative), absolute, temporary.filePath(relative), "/Disconnected/known.mp3"};
        for (int row = 0; row < paths.size(); ++row) {
            QVERIFY(songs.next());
            QCOMPARE(songs.value(0).toString(), paths[row]);
            QCOMPARE(songs.value(1).toString(), row == 3 ? QString("Preserved") : QStringLiteral("O'Connor & \"Night\" 夜"));
            QCOMPARE(songs.value(2).toString(), row == 3 ? QString("Offline") : QString("Don't \"Stop\""));
            QCOMPARE(songs.value(3).toInt(), row == 3 ? 123 : 2);
        }
        QVERIFY(!songs.next());
        QCOMPARE(QSqlDatabase::connectionNames().size(), 1);
    }

    void existingPlaylistAppendsWithoutRewriting()
    {
        addKnown();
        exec("INSERT INTO bmplaylists VALUES (7,'Existing')");
        exec("INSERT INTO bmplsongs VALUES (55,7,8,101,101,101,101,101)");
        writePlaylist({"/Disconnected/known.mp3", "/Disconnected/known.mp3"});
        auto target = request();
        target.playlistTitle = "existing";
        BreakPlaylistImporter importer;
        QSignalSpy finished(&importer, &BreakPlaylistImporter::finished);
        QVERIFY(importer.start(target));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished[0][0].value<BreakPlaylistImportResult>();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.playlistId, 7);
        QCOMPARE(result.imported, 2);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplaylists").toInt(), 1);
        QCOMPARE(scalar("SELECT position FROM bmplsongs WHERE plsongid=55").toInt(), 8);
        QCOMPARE(scalar("SELECT MAX(position) FROM bmplsongs WHERE playlist=7").toInt(), 10);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplsongs WHERE playlist=7").toInt(), 3);
    }

    void failureRollsBackSongsAndPlaylist()
    {
        writePlaylist({relative, relative});
        exec("CREATE TRIGGER reject_import BEFORE INSERT ON bmplsongs WHEN NEW.position=1 "
             "BEGIN SELECT RAISE(ABORT,'test rollback'); END");
        BreakPlaylistImporter importer;
        QSignalSpy finished(&importer, &BreakPlaylistImporter::finished);
        QVERIFY(importer.start(request()));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        const auto result = finished[0][0].value<BreakPlaylistImportResult>();
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplaylists").toInt(), 0);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 0);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplsongs").toInt(), 0);
    }

    void cancellationKeepsUiResponsive()
    {
        writePlaylist({relative});
        exec("BEGIN IMMEDIATE"); // Force the worker to wait for a write lock after metadata reading.
        const auto rollback = qScopeGuard([] { QSqlDatabase::database().rollback(); });
        BreakPlaylistImporter importer;
        QSignalSpy finished(&importer, &BreakPlaylistImporter::finished);
        bool heartbeat = false;
        QTimer::singleShot(50, &importer, [&] { heartbeat = true; importer.cancel(); });
        QVERIFY(importer.start(request()));
        QVERIFY(!importer.start(request()));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(heartbeat);
        const auto result = finished[0][0].value<BreakPlaylistImportResult>();
        QVERIFY(result.cancelled && result.error.isEmpty());
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplaylists").toInt(), 0);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 0);
    }

    void deletedDestinationDoesNotRecreateIt()
    {
        addKnown();
        exec("INSERT INTO bmplaylists VALUES (7,'Existing')");
        writePlaylist({"/Disconnected/known.mp3"});
        auto target = request();
        target.playlistId = 7;
        BreakPlaylistImporter importer;
        QSignalSpy finished(&importer, &BreakPlaylistImporter::finished);
        {
            WorkerGate gate;
            QVERIFY(importer.start(target));
            exec("DELETE FROM bmplaylists");
        }
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished[0][0].value<BreakPlaylistImportResult>().error.isEmpty());
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplaylists").toInt(), 0);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplsongs").toInt(), 0);
    }

    void destructionAndUnreadablePlaylist()
    {
        writePlaylist({relative});
        {
            WorkerGate gate;
            auto importer = std::make_unique<BreakPlaylistImporter>();
            QVERIFY(importer->start(request()));
            importer.reset();
        }
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplaylists").toInt(), 0);
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmsongs").toInt(), 0);
        auto missing = request();
        missing.filename = temporary.filePath("not-there.m3u");
        BreakPlaylistImporter importer;
        QSignalSpy finished(&importer, &BreakPlaylistImporter::finished);
        QVERIFY(importer.start(missing));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished[0][0].value<BreakPlaylistImportResult>().error.isEmpty());
        QCOMPARE(scalar("SELECT COUNT(*) FROM bmplaylists").toInt(), 0);
        QCOMPARE(QSqlDatabase::connectionNames().size(), 1);
        finished.clear();
        auto invalid = request();
        invalid.playlistTitle.clear();
        QVERIFY(importer.start(invalid));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished[0][0].value<BreakPlaylistImportResult>().error.isEmpty());
        finished.clear();
        invalid = request();
        invalid.databasePath = temporary.filePath("absent.sqlite");
        QVERIFY(importer.start(invalid));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY(!finished[0][0].value<BreakPlaylistImportResult>().error.isEmpty());
        QVERIFY(!QFile::exists(invalid.databasePath));
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

QTEST_MAIN(PlaylistImportTest)
#include "playlist_import_test.moc"
