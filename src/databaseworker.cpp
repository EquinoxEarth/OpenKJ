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
    
    // Set fast SQLite pragmas
    QSqlQuery query(m_db);
    query.exec("PRAGMA synchronous=OFF");
    query.exec("PRAGMA cache_size=300000");
    query.exec("PRAGMA temp_store=2");
}

void DatabaseWorker::loadRotation()
{
    QList<okj::RotationSinger> singers;
    QSqlQuery query(m_db);
    
    // 1. Load basic singer details
    if (!query.exec("SELECT singerid, name, position, regular, addts FROM rotationsingers ORDER BY position")) {
        emit errorOccurred(query.lastError().text());
        return;
    }
    while (query.next()) {
        okj::RotationSinger s(
            query.value(0).toInt(),
            query.value(1).toString(),
            query.value(2).toInt(),
            query.value(3).toBool(),
            query.value(4).toDateTime()
        );
        singers.append(s);
    }
    
    // 2. Load numSongsSung counts
    query.exec("SELECT singer, COUNT(qsongid) FROM queuesongs WHERE played = 1 GROUP BY singer");
    while (query.next()) {
        int singerId = query.value(0).toInt();
        int count = query.value(1).toInt();
        for (auto &s : singers) {
            if (s.id == singerId) {
                s.cachedNumSongsSung = count;
                break;
            }
        }
    }
    
    // 3. Load numSongsUnsung counts
    query.exec("SELECT singer, COUNT(qsongid) FROM queuesongs WHERE played = 0 GROUP BY singer");
    while (query.next()) {
        int singerId = query.value(0).toInt();
        int count = query.value(1).toInt();
        for (auto &s : singers) {
            if (s.id == singerId) {
                s.cachedNumSongsUnsung = count;
                break;
            }
        }
    }
    
    // 4. Load next song details
    query.exec("SELECT q.singer, d.path, d.artist, d.title, d.discid, d.duration, q.keychg, q.qsongid "
               "FROM queuesongs q "
               "INNER JOIN dbsongs d ON d.songid = q.song "
               "WHERE q.played = 0 "
               "AND q.position = ("
               "    SELECT MIN(q2.position) "
               "    FROM queuesongs q2 "
               "    WHERE q2.singer = q.singer AND q2.played = 0"
               ")");
    while (query.next()) {
        int singerId = query.value(0).toInt();
        for (auto &s : singers) {
            if (s.id == singerId) {
                s.cachedNextSongPath = query.value(1).toString();
                s.cachedNextSongArtist = query.value(2).toString();
                s.cachedNextSongTitle = query.value(3).toString();
                s.cachedNextSongSongId = query.value(4).toString();
                s.cachedNextSongDurationSecs = (query.value(5).toInt() / 1000) + m_settings.estimationSingerPad();
                s.cachedNextSongKeyChg = query.value(6).toInt();
                s.cachedNextSongQueueId = query.value(7).toInt();
                break;
            }
        }
    }
    
    // Handle estimated empty song length for empty singers
    for (auto &s : singers) {
        if (s.cachedNextSongPath.isEmpty()) {
            if (!m_settings.estimationSkipEmptySingers()) {
                s.cachedNextSongDurationSecs = m_settings.estimationEmptySongLength() + m_settings.estimationSingerPad();
            } else {
                s.cachedNextSongDurationSecs = 0;
            }
        }
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
                  "ORDER BY queuesongs.position");
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
