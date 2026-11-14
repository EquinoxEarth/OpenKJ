#include "databaseworker.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QDateTime>
#include <spdlog/spdlog.h>
#include <chrono>
#include <QMap>
#include <QSet>
#include <stdexcept>

namespace {
void execute(QSqlQuery &query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}
void execute(QSqlQuery &query, const QString &sql)
{
    if (!query.exec(sql))
        throw std::runtime_error(query.lastError().text().toStdString());
}
using Records = QMap<int, QVariantList>;
Records records(QSqlDatabase &db, const QString &sql)
{
    QSqlQuery query(db);
    execute(query, sql);
    Records result;
    while (query.next()) {
        QVariantList row;
        for (int i = 1; i < query.record().count(); ++i)
            row.append(query.value(i));
        result.insert(query.value(0).toInt(), row);
    }
    if (query.lastError().isValid())
        throw std::runtime_error(query.lastError().text().toStdString());
    return result;
}
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
}

// All writes, including snapshot validation, run in one transaction. Never emit
// success before COMMIT: a failed statement or deferred constraint rolls back.
bool DatabaseWorker::write(int singerId, const std::function<void()> &operation)
{
    if (!m_db.transaction()) {
        const QString error = m_db.lastError().text();
        emit writeFailed(singerId);
        emit errorOccurred(error);
        return false;
    }
    try {
        operation();
        if (!m_db.commit())
            throw std::runtime_error(m_db.lastError().text().toStdString());
    } catch (const std::exception &error) {
        const bool rolledBack = m_db.rollback();
        const QString message = QString::fromUtf8(error.what()) +
            (rolledBack ? QString() : QString("; rollback failed: ") + m_db.lastError().text());
        emit writeFailed(singerId);
        emit errorOccurred(message);
        return false;
    }
    return true;
}

DatabaseWorker::DatabaseWorker(const QString &dbPath, QObject *parent)
    : QObject(parent), m_dbPath(dbPath)
{
}

DatabaseWorker::~DatabaseWorker()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
}

void DatabaseWorker::init()
{
    m_db = QSqlDatabase::addDatabase("QSQLITE", "openkj_background");
    m_db.setDatabaseName(m_dbPath);
    if (!m_db.open()) {
        spdlog::get("logger")->error("[DatabaseWorker] Failed to open background database connection: {}",
                                    m_db.lastError().text().toStdString());
        emit errorOccurred(m_db.lastError().text());
        return;
    }
    
    QSqlQuery query(m_db);
    if (!query.exec("PRAGMA synchronous=FULL")) {
        const auto error = query.lastError().text();
        query.finish();
        m_db.close();
        emit errorOccurred(error);
        return;
    }
    query.exec("PRAGMA cache_size=300000");
    query.exec("PRAGMA temp_store=2");
}

void DatabaseWorker::loadRotation()
{
    QList<okj::RotationSinger> singers;
    QSqlQuery query(m_db);
    // Each singer needs two index range counts and at most one next row.
    if (!query.exec(
            "SELECT r.singerid, r.name, r.position, r.regular, r.addts, "
            "(SELECT COUNT(*) FROM queueSongs WHERE singer = r.singerid AND played = 1), "
            "(SELECT COUNT(*) FROM queueSongs WHERE singer = r.singerid AND played = 0), "
            "d.path, d.artist, d.title, d.discid, d.duration, q.keychg, q.qsongid "
            "FROM rotationSingers r "
            "LEFT JOIN queueSongs q ON q.qsongid = ("
            "  SELECT next.qsongid FROM queueSongs next "
            "  WHERE next.singer = r.singerid AND next.played = 0 AND next.position IS NOT NULL "
            "  ORDER BY next.position, next.qsongid LIMIT 1"
            ") "
            "LEFT JOIN dbSongs d ON d.songid = q.song "
            "ORDER BY r.position, r.singerid")) {
        emit errorOccurred(query.lastError().text());
        return;
    }
    while (query.next()) {
        okj::RotationSinger singer(
            query.value(0).toInt(),
            query.value(1).toString(),
            query.value(2).toInt(),
            query.value(3).toBool(),
            query.value(4).toDateTime()
        );
        singer.cachedNumSongsSung = query.value(5).toInt();
        singer.cachedNumSongsUnsung = query.value(6).toInt();
        if (!query.value(7).isNull()) {
            singer.cachedNextSongPath = query.value(7).toString();
            singer.cachedNextSongArtist = query.value(8).toString();
            singer.cachedNextSongTitle = query.value(9).toString();
            singer.cachedNextSongSongId = query.value(10).toString();
            singer.cachedNextSongDurationMs = query.value(11).toInt();
            singer.cachedNextSongKeyChg = query.value(12).toInt();
            singer.cachedNextSongQueueId = query.value(13).toInt();
        }
        singers.append(singer);
    }
    if (query.lastError().isValid()) {
        emit errorOccurred(query.lastError().text());
        return;
    }

    emit rotationLoaded(singers);
}

void DatabaseWorker::loadQueue(int singerId, quint64 requestId)
{
    QList<okj::QueueSong> songs;
    QSqlQuery query(m_db);
    query.prepare("SELECT queuesongs.qsongid, queuesongs.singer, queuesongs.song, queuesongs.played, "
                  "queuesongs.keychg, queuesongs.position, rotationsingers.name, dbsongs.artist, "
                  "dbsongs.title, dbsongs.discid, dbsongs.duration, dbsongs.path FROM queuesongs "
                  "INNER JOIN rotationsingers ON rotationsingers.singerid = queuesongs.singer "
                  "INNER JOIN dbsongs ON dbsongs.songid = queuesongs.song WHERE queuesongs.singer = :singerId "
                  "ORDER BY queuesongs.position, queuesongs.qsongid");
    query.bindValue(":singerId", singerId);
    if (!query.exec()) {
        emit errorOccurred(query.lastError().text());
        return;
    }
    while (query.next()) {
        songs.emplace_back(okj::QueueSong{
            query.value(0).toInt(),
            query.value(1).toInt(),
            query.value(2).toInt(),
            query.value(3).toBool(),
            query.value(4).toInt(),
            query.value(5).toInt(),
            query.value(7).toString(),
            query.value(8).toString(),
            query.value(9).toString(),
            query.value(10).toInt(),
            query.value(11).toString()
        });
    }
    emit queueLoaded(singerId, requestId, songs);
}

void DatabaseWorker::addSinger(const QString &name, int positionHint, bool makeRegular)
{
    int singerId = -1;
    if (!write(-1, [&] {
        QSqlQuery query(m_db);
        int addPos = 0;
        execute(query, "SELECT COUNT(singerid) FROM rotationsingers");
        if (query.next()) {
            addPos = query.value(0).toInt();
        }
    
        QDateTime curTs = QDateTime::currentDateTime();
        query.prepare("INSERT INTO rotationsingers (name, position, regular, regularid, addts) "
                      "VALUES (:name, :pos, :regular, -1, :addts)");
        query.bindValue(":name", name);
        query.bindValue(":pos", addPos);
        query.bindValue(":regular", makeRegular);
        query.bindValue(":addts", curTs);
        execute(query);
        singerId = query.lastInsertId().toInt();
    
    })) return;
    emit singerAdded(singerId, name, positionHint);
    loadRotation();
}

void DatabaseWorker::addSingerWithSong(const QString &name, int positionHint, int songId, int keyChange, bool makeRegular)
{
    int singerId = -1;
    if (!write(-1, [&] {
        QSqlQuery query(m_db);
        int addPos = 0;
        execute(query, "SELECT COUNT(singerid) FROM rotationsingers");
        if (query.next()) {
            addPos = query.value(0).toInt();
        }
    
        QDateTime curTs = QDateTime::currentDateTime();
        query.prepare("INSERT INTO rotationsingers (name, position, regular, regularid, addts) "
                      "VALUES (:name, :pos, :regular, -1, :addts)");
        query.bindValue(":name", name);
        query.bindValue(":pos", addPos);
        query.bindValue(":regular", makeRegular);
        query.bindValue(":addts", curTs);
        execute(query);
        singerId = query.lastInsertId().toInt();
    
        query.prepare("INSERT INTO queuesongs (singer, song, artist, title, discid, path, keychg, played, position) "
                      "VALUES (:singerId, :songId, :songId, :songId, :songId, :songId, :key, 0, 0)");
        query.bindValue(":singerId", singerId);
        query.bindValue(":songId", songId);
        query.bindValue(":key", keyChange);
        execute(query);
    
    })) return;
    emit singerAdded(singerId, name, positionHint);
    emit queueUpdated(singerId);
    loadRotation();
}

void DatabaseWorker::moveSinger(int oldPosition, int newPosition)
{
    // Sorting and updating position is handled by TableModelRotation::singerMove in memory,
    // which then calls commitRotation. So this slot isn't strictly needed if we use commitRotation.
}

void DatabaseWorker::renameSinger(int singerId, const QString &newName)
{
    if (!write(singerId, [&] {
        QSqlQuery query(m_db);
        query.prepare("UPDATE rotationsingers SET name = :name WHERE singerid = :singerid");
        query.bindValue(":name", newName);
        query.bindValue(":singerid", singerId);
        execute(query);
    })) return;
    loadRotation();
}

void DatabaseWorker::deleteSinger(int singerId)
{
    // The model updates in-memory and then commits via commitRotation.
}

void DatabaseWorker::setSingerRegular(int singerId, bool isRegular)
{
    if (!write(singerId, [&] {
        QSqlQuery query(m_db);
        query.prepare("UPDATE rotationsingers SET regular = :regular WHERE singerid = :singerid");
        query.bindValue(":regular", isRegular);
        query.bindValue(":singerid", singerId);
        execute(query);
    })) return;
    loadRotation();
}

void DatabaseWorker::clearRotation()
{
    if (!write(-1, [&] {
        QSqlQuery query(m_db);
        execute(query, "DELETE FROM queuesongs");
        execute(query, "DELETE FROM rotationsingers");
    })) return;
    loadRotation();
}

void DatabaseWorker::addSongToQueue(int singerId, int songId, int keyChange)
{
    if (!write(singerId, [&] {
        // Use the destination queue's persisted order, independent of which singer
        // is currently displayed. Compute the append position in the same statement.
        QSqlQuery query(m_db);
        query.prepare("INSERT INTO queuesongs (singer, song, artist, title, discid, path, keychg, played, position) "
                      "SELECT :singerId, :songId, :songId, :songId, :songId, :songId, :key, 0, "
                      "COALESCE(MAX(position), -1) + 1 FROM queuesongs WHERE singer = :singerId");
        query.bindValue(":singerId", singerId);
        query.bindValue(":songId", songId);
        query.bindValue(":key", keyChange);
        execute(query);
    
    })) return;
    emit queueUpdated(singerId);
    loadRotation();
}

void DatabaseWorker::moveQueueSong(int singerId, int oldPosition, int newPosition)
{
    // The model handles sorting in-memory and commits via commitQueue.
}

void DatabaseWorker::deleteQueueSong(int singerId, int queueSongId)
{
    // Handled via commitQueue.
}

void DatabaseWorker::setQueueSongKey(int singerId, int songId, int key)
{
    if (!write(singerId, [&] {
        QSqlQuery query(m_db);
        query.prepare("UPDATE queuesongs SET keychg = :key WHERE qsongid = :id AND singer = :singerId");
        query.bindValue(":singerId", singerId);
        query.bindValue(":id", songId);
        query.bindValue(":key", key);
        execute(query);
    
    })) return;
    emit queueUpdated(singerId);
    loadRotation();
}

void DatabaseWorker::setQueueSongPlayed(int singerId, int songId, bool played)
{
    if (!write(singerId, [&] {
        QSqlQuery query(m_db);
        query.prepare("UPDATE queuesongs SET played = :played WHERE qsongid = :id AND singer = :singerId");
        query.bindValue(":singerId", singerId);
        query.bindValue(":id", songId);
        query.bindValue(":played", played);
        execute(query);
    
    })) return;
    emit queueUpdated(singerId);
    loadRotation();
}

void DatabaseWorker::recordKaraokePlay(const okj::QueueSong &song, const QString &historySinger,
                                       const QDateTime &playedAt)
{
    int plays = 0;
    if (!write(song.singerId, [&] {
        QSqlQuery query(m_db);
        if (song.dbSongId > 0) {
            query.prepare("UPDATE dbSongs SET plays = COALESCE(plays, 0) + 1, lastplay = :time "
                          "WHERE songid = :id");
            query.bindValue(":time", playedAt);
            query.bindValue(":id", song.dbSongId);
            execute(query);
            require(query.numRowsAffected() == 1, "The played song is no longer in the library.");
            query.prepare("SELECT plays FROM dbSongs WHERE songid = :id");
            query.bindValue(":id", song.dbSongId);
            execute(query);
            require(query.next(), "Could not read the saved play count.");
            plays = query.value(0).toInt();
            query.finish();
        }

        if (!historySinger.isEmpty()) {
            query.prepare("INSERT INTO historySingers (name) SELECT :name "
                          "WHERE NOT EXISTS (SELECT 1 FROM historySingers WHERE name = :name)");
            query.bindValue(":name", historySinger);
            execute(query);
            query.prepare("UPDATE historySongs SET artist = :artist, title = :title, songid = :songid, "
                          "keychange = :key, plays = COALESCE(plays, 0) + 1, lastplay = :time "
                          "WHERE filepath = :path AND historySinger = (SELECT id FROM historySingers WHERE name = :name)");
            const auto bindHistory = [&] {
                query.bindValue(":artist", song.artist);
                query.bindValue(":title", song.title);
                query.bindValue(":songid", song.songId);
                query.bindValue(":key", song.keyChange);
                query.bindValue(":time", playedAt);
                query.bindValue(":path", song.path);
                query.bindValue(":name", historySinger);
            };
            bindHistory();
            execute(query);
            if (query.numRowsAffected() == 0) {
                query.prepare("INSERT INTO historySongs (historySinger, filepath, artist, title, songid, keychange, plays, lastplay) "
                              "SELECT id, :path, :artist, :title, :songid, :key, 1, :time "
                              "FROM historySingers WHERE name = :name");
                bindHistory();
                execute(query);
                require(query.numRowsAffected() == 1, "Could not save the singer's play history.");
            }
        }

        if (song.id > 0) {
            query.prepare("UPDATE queueSongs SET played = 1 WHERE qsongid = :id AND singer = :singer AND song = :song");
            query.bindValue(":id", song.id);
            query.bindValue(":singer", song.singerId);
            query.bindValue(":song", song.dbSongId);
            execute(query);
            require(query.numRowsAffected() == 1, "The played song is no longer in the singer's queue.");
        }
    }))
        return;

    emit karaokePlayRecorded(song, historySinger, plays, playedAt);
    if (song.id > 0) {
        emit queueUpdated(song.singerId);
        loadRotation();
    }
}

void DatabaseWorker::removeAllQueueSongs(int singerId)
{
    if (!write(singerId, [&] {
        QSqlQuery query(m_db);
        query.prepare("DELETE FROM queuesongs WHERE singer = :singerId");
        query.bindValue(":singerId", singerId);
        execute(query);
    
    })) return;
    emit queueUpdated(singerId);
    loadRotation();
}

void DatabaseWorker::commitRotation(const QList<okj::RotationSinger> &singers,
                                    const QList<okj::RotationSinger> &expected)
{
    if (!write(-1, [&] {
        Records baseline;
        for (const auto &s : expected)
            baseline.insert(s.id, {s.name, s.position, int(s.regular), s.addTs.toString(Qt::ISODateWithMs)});
        auto current = records(m_db, "SELECT singerid, name, position, regular, addts FROM rotationsingers");
        // Normalize timestamps through the same conversion as loadRotation().
        for (auto &row : current) {
            row[1] = row[1].toInt();
            row[2] = row[2].toInt();
            row[3] = row[3].toDateTime().toString(Qt::ISODateWithMs);
        }
        require(current == baseline, "Rotation changed while editing. Reloaded the saved rotation; please retry.");
        QSet<int> retained;
        QSqlQuery query(m_db);
        for (const auto &s : singers) {
            require(baseline.contains(s.id) && !retained.contains(s.id), "Invalid rotation snapshot");
            retained.insert(s.id);
            query.prepare("UPDATE rotationsingers SET position = :pos WHERE singerid = :id");
            query.bindValue(":pos", s.position);
            query.bindValue(":id", s.id);
            execute(query);
        }
        for (const auto &s : expected) {
            if (retained.contains(s.id)) continue;
            query.prepare("DELETE FROM queuesongs WHERE singer = :id");
            query.bindValue(":id", s.id);
            execute(query);
            query.prepare("DELETE FROM rotationsingers WHERE singerid = :id");
            query.bindValue(":id", s.id);
            execute(query);
        }
    })) return;
    loadRotation();
}

void DatabaseWorker::commitQueue(int singerId, const QList<okj::QueueSong> &songs,
                                 const QList<okj::QueueSong> &expected)
{
    if (!write(singerId, [&] {
        Records baseline;
        for (const auto &song : expected) {
            require(song.singerId == singerId, "Invalid queue owner");
            baseline.insert(song.id, {song.dbSongId, int(song.played), song.keyChange, song.position});
        }
        auto current = records(m_db, QString("SELECT qsongid, song, played, keychg, position FROM queuesongs WHERE singer = %1").arg(singerId));
        for (auto &row : current)
            for (auto &value : row) value = value.toInt();
        require(current == baseline, "Queue changed while editing. Reloaded the saved queue; please retry.");
        QSet<int> retained;
        QSqlQuery query(m_db);
        for (const auto &song : songs) {
            require(song.singerId == singerId && baseline.contains(song.id) && !retained.contains(song.id), "Invalid queue snapshot");
            retained.insert(song.id);
            // Other fields have targeted setters; rebuilding must never replace them.
            query.prepare("UPDATE queuesongs SET position = :pos WHERE qsongid = :id AND singer = :singer");
            query.bindValue(":pos", song.position);
            query.bindValue(":id", song.id);
            query.bindValue(":singer", singerId);
            execute(query);
        }
        for (const auto &song : expected) {
            if (retained.contains(song.id)) continue;
            query.prepare("DELETE FROM queuesongs WHERE qsongid = :id AND singer = :singer");
            query.bindValue(":id", song.id);
            query.bindValue(":singer", singerId);
            execute(query);
        }
    })) return;
    emit queueUpdated(singerId);
    loadRotation();
}
