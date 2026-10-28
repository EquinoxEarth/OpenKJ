#include "breakplaylistimporter.h"
#include "tagreader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <stdexcept>

namespace {
void execute(QSqlQuery &query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}
void prepare(QSqlQuery &query, const QString &sql)
{
    if (!query.prepare(sql))
        throw std::runtime_error(query.lastError().text().toStdString());
}
int songId(QSqlQuery &lookup, const QString &path)
{
    lookup.bindValue(":path", path);
    execute(lookup);
    const int id = lookup.next() ? lookup.value(0).toInt() : -1;
    lookup.finish();
    return id;
}
struct Song {
    QString artist, title, filename;
    int duration = 0;
    bool known = false;
};
}

BreakPlaylistImporter::BreakPlaylistImporter(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<BreakPlaylistImportResult>();
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] {
        emit progressChanged(m_state->processed.load(), m_state->total.load());
    });
    connect(&m_watcher, &QFutureWatcher<BreakPlaylistImportResult>::finished, this, [this] {
        m_progressTimer.stop();
        const auto result = m_watcher.future().takeResult();
        m_running = false;
        m_state.reset();
        emit finished(result);
    });
}

BreakPlaylistImporter::~BreakPlaylistImporter()
{
    cancel(); // Jobs own their inputs and connection; destruction never waits for file I/O.
}

bool BreakPlaylistImporter::start(const BreakPlaylistImportRequest &request)
{
    if (m_running)
        return false;
    m_running = true;
    m_state = std::make_shared<State>();
    m_progressTimer.start();
    m_watcher.setFuture(QtConcurrent::run([request, state = m_state] { return run(request, state); }));
    return true;
}

void BreakPlaylistImporter::cancel()
{
    if (m_state)
        m_state->cancelled = true;
}

BreakPlaylistImportResult BreakPlaylistImporter::run(const BreakPlaylistImportRequest &request,
                                                     const std::shared_ptr<State> &state)
{
    BreakPlaylistImportResult result;
    const auto cancelled = [&] {
        result.cancelled = state->cancelled.load();
        return result.cancelled;
    };
    if (cancelled())
        return result;
    // Construct, use and destroy SQLite and TagReader objects entirely on this worker.
    const auto connection = "playlist-import-" + QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(request.databasePath);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
        try {
            const auto import = [&] {
                if (!QFileInfo(request.databasePath).isFile())
                    throw std::runtime_error("The music database is unavailable.");
                if (request.playlistId < 0 && request.playlistTitle.isEmpty())
                    throw std::runtime_error("A playlist title is required.");
                if (!db.open())
                    throw std::runtime_error(db.lastError().text().toStdString());
                QFile file(request.filename);
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                    throw std::runtime_error(file.errorString().toStdString());
                QTextStream stream(&file);
                QStringList entries;
                while (!stream.atEnd()) {
                    if (cancelled())
                        return;
                    auto line = stream.readLine();
                    if (!line.isEmpty() && !line.startsWith('#'))
                        entries.append(line.replace('\\', '/'));
                }
                if (file.error() != QFileDevice::NoError)
                    throw std::runtime_error(file.errorString().toStdString());
                state->total = entries.size();
                const auto directory = QFileInfo(request.filename).absolutePath();
                QSqlQuery lookup(db);
                prepare(lookup, "SELECT songid FROM bmsongs WHERE path=:path");
                QHash<QString, Song> songs;
                QStringList orderedPaths;
                TagReader reader;
                for (const auto &entry : entries) {
                    if (cancelled())
                        return;
                    QStringList candidates{entry};
                    if (!QDir::isAbsolutePath(entry))
                        candidates.append(directory + '/' + entry);
                    QString resolved;
                    for (const auto &candidate : candidates) {
                        if (songs.contains(candidate)) {
                            resolved = candidate;
                            break;
                        }
                        // Known records on disconnected media remain usable and are never deleted.
                        if (songId(lookup, candidate) >= 0) {
                            songs.insert(candidate, Song{.known = true});
                            resolved = candidate;
                            break;
                        }
                        if (QFileInfo(candidate).isFile()) {
                            reader.setMedia(candidate);
                            if (cancelled())
                                return;
                            songs.insert(candidate, Song{reader.getArtist(), reader.getTitle(),
                                QFileInfo(candidate).fileName(), static_cast<int>(reader.getDuration() / 1000)});
                            resolved = candidate;
                            break;
                        }
                    }
                    if (resolved.isEmpty())
                        ++result.skipped;
                    else
                        orderedPaths.append(resolved); // Repetitions are intentional playlist entries.
                    ++state->processed;
                }
                if (cancelled())
                    return;

                // No disk metadata reads occur while holding SQLite's write lock.
                QSqlQuery transaction(db);
                if (!transaction.exec("BEGIN IMMEDIATE"))
                    throw std::runtime_error(transaction.lastError().text().toStdString());
                bool committed = false;
                const auto rollback = qScopeGuard([&] { if (!committed) db.rollback(); });
                QSqlQuery playlist(db);
                if (request.playlistId >= 0) {
                    prepare(playlist, "SELECT playlistid FROM bmplaylists WHERE playlistid=:id");
                    playlist.bindValue(":id", request.playlistId);
                    execute(playlist);
                    if (!playlist.next())
                        throw std::runtime_error("The destination playlist was removed during import.");
                    result.playlistId = request.playlistId;
                } else {
                    prepare(playlist, "SELECT playlistid FROM bmplaylists WHERE title=:title COLLATE NOCASE");
                    playlist.bindValue(":title", request.playlistTitle);
                    execute(playlist);
                    if (playlist.next())
                        result.playlistId = playlist.value(0).toInt();
                }
                playlist.finish();
                if (result.playlistId < 0) {
                    prepare(playlist, "INSERT INTO bmplaylists (title) VALUES (:title)");
                    playlist.bindValue(":title", request.playlistTitle);
                    execute(playlist);
                    result.playlistId = playlist.lastInsertId().toInt();
                }
                QHash<QString, int> ids;
                QSqlQuery insert(db);
                prepare(insert, "INSERT OR IGNORE INTO bmsongs (artist,title,path,filename,duration,searchstring) "
                                "VALUES (:artist,:title,:path,:filename,:duration,:search)");
                for (auto it = songs.cbegin(); it != songs.cend(); ++it) {
                    if (cancelled())
                        return;
                    int id = songId(lookup, it.key());
                    if (id < 0) {
                        if (it->known)
                            throw std::runtime_error("A library song was removed during import. Please try again.");
                        insert.bindValue(":artist", it->artist);
                        insert.bindValue(":title", it->title);
                        insert.bindValue(":path", it.key());
                        insert.bindValue(":filename", it->filename);
                        insert.bindValue(":duration", it->duration);
                        insert.bindValue(":search", it->artist + ' ' + it->title + ' ' + it->filename);
                        execute(insert);
                        id = songId(lookup, it.key());
                        if (id < 0)
                            throw std::runtime_error("Unable to add a playlist song to the library.");
                    }
                    ids.insert(it.key(), id);
                }
                prepare(playlist, "SELECT COALESCE(MAX(position),-1)+1 FROM bmplsongs WHERE playlist=:playlist");
                playlist.bindValue(":playlist", result.playlistId);
                execute(playlist);
                if (!playlist.next())
                    throw std::runtime_error("Unable to determine the playlist append position.");
                int position = playlist.value(0).toInt();
                playlist.finish();
                prepare(insert, "INSERT INTO bmplsongs (playlist,position,artist,title,filename,duration,path) "
                                "VALUES (:playlist,:position,:song,:song,:song,:song,:song)");
                for (const auto &path : orderedPaths) {
                    if (cancelled())
                        return;
                    insert.bindValue(":playlist", result.playlistId);
                    insert.bindValue(":position", position++);
                    insert.bindValue(":song", ids.value(path));
                    execute(insert);
                }
                if (cancelled())
                    return;
                if (!db.commit())
                    throw std::runtime_error(db.lastError().text().toStdString());
                committed = true;
                result.imported = orderedPaths.size();
            };
            import();
        } catch (const std::exception &error) {
            if (!cancelled())
                result.error = QString::fromUtf8(error.what());
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}
