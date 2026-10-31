/*
 * Copyright (c) 2013-2019 Thomas Isaac Lightburn
 *
 *
 * This file is part of OpenKJ.
 *
 * OpenKJ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bmdbupdatethread.h"
#include "directorylisting.h"
#include "tagreader.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <stdexcept>

namespace {
void execute(QSqlQuery &query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}
struct Song {
    QString path, artist, title, filename;
    int duration;
};
}

BmDbUpdater::BmDbUpdater(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<BmDbUpdateResult>();
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, &BmDbUpdater::reportProgress);
    connect(&m_watcher, &QFutureWatcher<BmDbUpdateResult>::finished, this, [this] {
        m_progressTimer.stop();
        const auto result = m_watcher.future().takeResult();
        reportProgress();
        m_state.reset();
        m_running = false;
        emit finished(result);
    });
}

BmDbUpdater::~BmDbUpdater()
{
    cancel(); // Workers own their inputs and never wait for the UI.
}

bool BmDbUpdater::start(const QString &databasePath, const QStringList &paths)
{
    if (m_running || paths.isEmpty())
        return false;
    m_running = true;
    m_state = std::make_shared<State>();
    m_lastStatus.clear();
    m_lastMessage.clear();
    m_progressTimer.start();
    m_watcher.setFuture(QtConcurrent::run([databasePath, paths, state = m_state] {
        return run(databasePath, paths, state);
    }));
    return true;
}

void BmDbUpdater::cancel()
{
    if (m_state)
        m_state->cancelled = true;
}

void BmDbUpdater::reportProgress()
{
    QString status, message;
    {
        const QMutexLocker lock(&m_state->mutex);
        status = m_state->status;
        message = m_state->message;
    }
    if (m_state->cancelled)
        status = tr("Cancelling update…");
    if (status != m_lastStatus) {
        m_lastStatus = status;
        emit stateChanged(status);
    }
    if (message != m_lastMessage) {
        m_lastMessage = message;
        emit progressMessage(message);
    }
    emit progressChanged(m_state->processed.load(), m_state->total.load());
}

BmDbUpdateResult BmDbUpdater::run(const QString &databasePath, const QStringList &paths,
                                 const std::shared_ptr<State> &state)
{
    BmDbUpdateResult result;
    const auto cancelled = [&] {
        result.cancelled = state->cancelled.load();
        return result.cancelled;
    };
    const auto publish = [&](const QString &status, const QString &message) {
        const QMutexLocker lock(&state->mutex);
        state->status = status;
        state->message = message;
    };
    if (cancelled())
        return result;
    const auto connection = "break-scan-" + QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(databasePath);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
        try {
            const auto scan = [&] {
                if (!QFileInfo(databasePath).isFile())
                    throw std::runtime_error("The music database is unavailable.");
                if (!db.open())
                    throw std::runtime_error(db.lastError().text().toStdString());
                const QSet<QString> extensions{"mp3", "wav", "ogg", "flac", "m4a", "mkv",
                                                "avi", "mp4", "mpg", "mpeg", "wmv", "wma"};
                QStringList files;
                QSet<QString> seenFiles, seenRoots;
                for (const auto &path : paths) {
                    if (cancelled())
                        return;
                    if (path.isEmpty() || !QFileInfo(path).isDir() || !QFileInfo(path).isReadable()) {
                        result.unavailablePaths.append(path);
                        continue;
                    }
                    const auto root = QDir(path).absolutePath();
                    if (seenRoots.contains(root))
                        continue;
                    seenRoots.insert(root);
                    publish(tr("Finding media files…"), tr("Scanning %1").arg(root));
                    for (const auto &entry : okj::listFilesRecursively(root)) {
                        if (cancelled())
                            return;
                        const auto filename = entry.filePath();
                        if (extensions.contains(QFileInfo(filename).suffix().toLower()) && !seenFiles.contains(filename)) {
                            seenFiles.insert(filename);
                            files.append(filename);
                        }
                    }
                }
                result.total = files.size();
                state->total = result.total;
                // File lists now provide deduplication; do not retain two full path indexes.
                seenFiles.clear();
                QSqlQuery lookup(db);
                if (!lookup.prepare("SELECT 1 FROM bmsongs WHERE path=:path"))
                    throw std::runtime_error(lookup.lastError().text().toStdString());
                QList<Song> batch;
                constexpr qsizetype batchSize = 32;
                const auto flush = [&] {
                    if (batch.isEmpty())
                        return true;
                    if (cancelled())
                        return false;
                    publish(tr("Saving songs…"), tr("Writing a batch of %1 songs").arg(batch.size()));
                    QSqlQuery query(db);
                    if (!query.exec("BEGIN IMMEDIATE"))
                        throw std::runtime_error(query.lastError().text().toStdString());
                    bool committed = false;
                    const auto rollback = qScopeGuard([&] { if (!committed) db.rollback(); });
                    if (!query.prepare("INSERT OR IGNORE INTO bmsongs (artist,title,path,filename,duration,searchstring) "
                                       "VALUES (:artist,:title,:path,:filename,:duration,:search)"))
                        throw std::runtime_error(query.lastError().text().toStdString());
                    qint64 inserted = 0;
                    for (const auto &song : batch) {
                        if (cancelled())
                            return false;
                        query.bindValue(":artist", song.artist);
                        query.bindValue(":title", song.title);
                        query.bindValue(":path", song.path);
                        query.bindValue(":filename", song.filename);
                        query.bindValue(":duration", song.duration);
                        query.bindValue(":search", song.artist + song.title + song.path);
                        execute(query);
                        inserted += query.numRowsAffected();
                    }
                    if (cancelled())
                        return false;
                    if (!db.commit())
                        throw std::runtime_error(db.lastError().text().toStdString());
                    committed = true;
                    result.added += inserted;
                    result.existing += batch.size() - inserted;
                    batch.clear();
                    return true;
                };
                TagReader reader; // Constructed and used only on the worker thread.
                for (const auto &path : files) {
                    if (cancelled())
                        return;
                    publish(tr("Reading song metadata…"), tr("Processing %1").arg(path));
                    lookup.bindValue(":path", path);
                    execute(lookup);
                    const bool known = lookup.next();
                    lookup.finish();
                    if (known) {
                        ++result.existing;
                    } else if (!QFileInfo(path).isFile()) {
                        ++result.unavailableFiles;
                    } else {
                        reader.setMedia(path);
                        if (cancelled())
                            return;
                        batch.append({path, reader.getArtist(), reader.getTitle(), path,
                                      static_cast<int>(reader.getDuration() / 1000)});
                    }
                    ++result.processed;
                    state->processed = result.processed;
                    if (batch.size() >= batchSize && !flush())
                        return;
                }
                flush();
            };
            scan();
        } catch (const std::exception &error) {
            if (!cancelled())
                result.error = QString::fromUtf8(error.what());
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}
