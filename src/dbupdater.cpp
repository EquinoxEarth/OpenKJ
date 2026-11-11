#define SQL(...) #__VA_ARGS__
#include "dbupdater.h"
#include <array>
#include <QSqlQuery>
#include <QElapsedTimer>
#include <spdlog/spdlog.h>
#include <QFileInfo>
#include <QDir>
#include "directorylisting.h"
#include <QStandardPaths>
#include <QScopeGuard>
#include <QSet>
#include <QtConcurrentRun>
#include <QUuid>
#include <memory>
#include "mzarchive.h"
#include "karaokefileinfo.h"
#include "okjutil.h"
#include "settings.h"

namespace {
DbUpdater::ImportSettings currentImportSettings()
{
    Settings settings;
    return {settings.dbLazyLoadDurations(), settings.dbSkipValidation()};
}
}

DbUpdater::DbUpdater(QObject *parent)
    : QObject(parent), m_db(QSqlDatabase::database()), m_cancelled(nullptr),
      m_importSettings(currentImportSettings())
{
}

DbUpdater::DbUpdater(const QSqlDatabase &db, const std::atomic_bool *cancelled, ImportSettings settings)
    : m_db(db), m_cancelled(cancelled), m_importSettings(settings)
{
}

namespace {
template <typename Operation>
QFuture<DbUpdater::ScanResult> runScanJob(const QString &databasePath,
                                         const std::shared_ptr<DbUpdater::ScanState> &state,
                                         Operation operation)
{
    return QtConcurrent::run([databasePath, state, operation, settings = currentImportSettings()] {
        DbUpdater::ScanResult result;
        if (state->cancelled.load()) {
            result.cancelled = true;
            return result;
        }
        const auto name = QStringLiteral("karaoke_scan_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto removeConnection = qScopeGuard([&] { QSqlDatabase::removeDatabase(name); });
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(databasePath);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
        if (databasePath.isEmpty() || !db.open()) {
            result.errors.append(QObject::tr("Could not open the library database: %1").arg(db.lastError().text()));
            return result;
        }

        DbUpdater updater(db, &state->cancelled, settings);
        QObject::connect(&updater, &DbUpdater::stateChanged, &updater, [state](const QString &status) {
            QMutexLocker lock(&state->mutex);
            state->progress.status = status;
            state->progress.value = 0;
            state->progress.maximum = 0;
        });
        QObject::connect(&updater, &DbUpdater::progressChanged, &updater, [state](int value, int maximum) {
            QMutexLocker lock(&state->mutex);
            state->progress.value = value;
            state->progress.maximum = maximum;
        });
        result.success = operation(updater);
        result.cancelled = !result.success && state->cancelled.load();
        result.errors = updater.getErrors();
        result.missing = updater.missingFiles();
        return result;
    });
}
}

QFuture<DbUpdater::ScanResult> DbUpdater::scanAsync(const QString &databasePath, const QStringList &paths,
                                                   ProcessingOptions options, std::shared_ptr<ScanState> state)
{
    return runScanJob(databasePath, state, [paths, options](DbUpdater &updater) {
        return updater.process(paths, options);
    });
}

QFuture<DbUpdater::ScanResult> DbUpdater::removeMissingAsync(const QString &databasePath, const QStringList &paths,
                                                            const QList<MissingFile> &files,
                                                            std::shared_ptr<ScanState> state)
{
    return runScanJob(databasePath, state, [paths, files](DbUpdater &updater) {
        return updater.removeMissingFilesFromDatabase(files, paths);
    });
}

// Compare the disk and database paths before opening a write transaction.
bool DbUpdater::process(const QList<QString> &paths, ProcessingOptions options)
{
    m_errors.clear();
    m_missingFiles.clear();
    if (cancelled())
        return false;
    if (paths.isEmpty())
        return true;
    if (paths.contains(QString())) {
        m_errors.append(tr("A source folder has an empty path."));
        return false;
    }
    auto availablePaths = paths;
    if (options.testFlag(SkipUnavailableSources)) {
        availablePaths.removeIf([](const QString &path) {
            const QFileInfo source(path);
            return !source.isDir() || !source.isReadable();
        });
        if (availablePaths.isEmpty())
            return !cancelled();
    }
    setPaths(availablePaths);
    if (!sourcesAvailable())
        return false;

    emit stateChanged("Scanning disk for files...");
    DiskEnumerator diskEnumerator(*this);
    if (!diskEnumerator.findKaraokeFilesOnDisk())
        return false;

    emit stateChanged("Scanning database for files...");
    DbEnumerator dbEnumerator(*this);
    if (!dbEnumerator.prepareQuery(!options.testFlag(FixMovedFilesSearchInWholeDB)))
        return false;

    emit stateChanged("Checking files against database...");
    qInfo() << "Checking for new songs";
    emit progressChanged(0, 0);

    QStringList newFilesOnDisk; newFilesOnDisk.reserve(20000);
    QVector<DbSongRecord> filesMissingOnDisk;
    bool keepTrackOfMissing = options.testFlag(FixMovedFiles) || options.testFlag(PrepareForRemovalOfMissing);

    int run = 0;
    do {
        if (cancelled())
            return false;
        // Comparison result:
        //   when negative: file is on disk but not in database.
        //   when positive: file is in database but not on disk.
        //   when 0: file is on disk AND in database.
        int comp_result = 0;

        if (run > 0) {
            if (diskEnumerator.IsValid && dbEnumerator.IsValid) {
                comp_result = QString::compare(diskEnumerator.CurrentFile, dbEnumerator.CurrentRecord.path);
            }
            else {
                comp_result = (int)(dbEnumerator.IsValid) - (int)(diskEnumerator.IsValid);
            }

            if (comp_result < 0) {
                newFilesOnDisk.append(diskEnumerator.CurrentFile);
            }

            if (comp_result > 0 && keepTrackOfMissing) {
                filesMissingOnDisk.append(dbEnumerator.CurrentRecord);
            }

            if (comp_result == 0 && dbEnumerator.CurrentRecord.isDropped) {
                // Add drag'n'dropped files to the list of new songs so they
                // will be properly added (upserted) to the database.
                newFilesOnDisk.append(diskEnumerator.CurrentFile);
            }
        }

        if (comp_result <= 0) {
            diskEnumerator.readNextDiskFile();
        }

        if (comp_result >= 0) {
            dbEnumerator.readNextRecord();
        }
        run++;

    }
    while (diskEnumerator.IsValid || dbEnumerator.IsValid);
    dbEnumerator.finish();
    if (!m_errors.isEmpty())
        return false;


    QList<ImportRecord> imports;
    if (!sourcesAvailable() || !prepareImports(newFilesOnDisk, imports) ||
        cancelled() || !sourcesAvailable())
        return false;

    const bool detectMoves = options.testFlag(FixMovedFiles) &&
                             !newFilesOnDisk.isEmpty() && !filesMissingOnDisk.isEmpty();
    if (detectMoves || !imports.isEmpty()) {
        emit stateChanged(tr("Saving library changes..."));
        if (!m_db.transaction())
            return reportSqlError(m_db.lastError());
        auto rollback = qScopeGuard([this] { m_db.rollback(); });
        QSet<QString> moved;
        if (detectMoves && !fixMissingFiles(filesMissingOnDisk, newFilesOnDisk, moved))
            return false;
        if (!writeImports(imports, moved) || cancelled())
            return false;
        if (!m_db.commit())
            return reportSqlError(m_db.lastError());
        rollback.dismiss();
    }

    if (options.testFlag(PrepareForRemovalOfMissing)) {
        for (const auto &record : filesMissingOnDisk) {
            // Whole-database move detection can inspect other sources.
            if (std::any_of(m_paths.cbegin(), m_paths.cend(), [&record](const QString &prefix) {
                    return record.path.startsWith(prefix, Qt::CaseSensitive);
                }))
                m_missingFiles.append({record.id, record.path});
        }
    }
    return true;
}

bool DbUpdater::sourcesAvailable()
{
    for (const auto &path : m_paths) {
        const QFileInfo source(path);
        if (!source.isDir() || !source.isReadable()) {
            m_errors.append(tr("Source folder is unavailable or unreadable: %1").arg(path));
            return false;
        }
    }
    return true;
}

bool DbUpdater::prepareImports(const QStringList &files, QList<ImportRecord> &imports)
{
    if (files.isEmpty())
        return !cancelled();

    emit stateChanged(tr("Reading new song files..."));
    emit progressChanged(0, files.size());
    MzArchive archive;
    KaraokeFileInfo parser(this, std::make_shared<KaraokeFilePatternResolver>(m_db));
    int processed = 0;
    for (const auto &path : files) {
        if (cancelled())
            return false;
        if (progressDue())
            emit progressChanged(processed, files.size());
        ++processed;
#ifdef Q_OS_WIN
        if (path.contains("*") || path.contains("?") || path.contains("<") || path.contains(">") || path.contains("|")) {
            m_errors.append("Illegal character in filename: " + path);
            continue;
        }
#endif
        if (path.endsWith(".zip", Qt::CaseInsensitive) && !m_importSettings.skipValidation) {
            archive.setArchiveFile(path);
            if (!archive.isValidKaraokeFile()) {
                m_errors.append(archive.getLastError() + ": " + path);
                continue;
            }
        }
        parser.setFile(path);
        const int duration = m_importSettings.lazyDurations ? -2 : parser.getDuration();
        const auto discId = parser.getSongId();
        const auto artist = parser.getArtist();
        const auto title = parser.getTitle();
        const auto filename = QFileInfo(path).completeBaseName();
        imports.append({path, filename, artist, parser.parsedSuccessfully() ? title : filename,
                        discId, filename + " " + artist + " " + title + " " + discId, duration});
    }
    return !cancelled();
}

bool DbUpdater::writeImports(const QList<ImportRecord> &imports, const QSet<QString> &moved)
{
    if (imports.isEmpty())
        return true;
    QSqlQuery query(m_db);
    if (!query.prepare(SQL(
        INSERT INTO dbSongs (discid, artist, title, path, filename, duration, searchstring)
        VALUES(:discid, :artist, :title, :path, :filename, :duration, :searchstring)
        ON CONFLICT(path) DO UPDATE SET
            discid = excluded.discid, artist = excluded.artist, title = excluded.title,
            filename = excluded.filename, duration = excluded.duration, searchstring = excluded.searchstring
    )))
        return reportSqlError(query.lastError());

    emit progressChanged(0, imports.size());
    int processed = 0;
    for (const auto &record : imports) {
        if (cancelled())
            return false;
        if (progressDue())
            emit progressChanged(processed, imports.size());
        ++processed;
        if (moved.contains(record.path))
            continue;
        query.bindValue(":discid", record.discId);
        query.bindValue(":artist", record.artist);
        query.bindValue(":title", record.title);
        query.bindValue(":path", record.path);
        query.bindValue(":filename", record.filename);
        query.bindValue(":duration", record.duration);
        query.bindValue(":searchstring", record.search);
        if (!query.exec())
            return reportSqlError(query.lastError());
    }
    return true;
}

bool DbUpdater::addFilesToDatabase(const QList<QString> &files)
{
    QList<ImportRecord> imports;
    if (!prepareImports(files, imports))
        return false;
    if (imports.isEmpty())
        return true;
    if (!m_db.transaction())
        return reportSqlError(m_db.lastError());
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    if (!writeImports(imports, {}) || cancelled())
        return false;
    if (!m_db.commit())
        return reportSqlError(m_db.lastError());
    rollback.dismiss();
    return true;
}

int DbUpdater::missingFilesCount()
{
    return m_missingFiles.size();
}

bool DbUpdater::removeMissingFilesFromDatabase()
{
    const bool success = removeMissingFilesFromDatabase(m_missingFiles, m_paths);
    if (success)
        m_missingFiles.clear();
    return success;
}

bool DbUpdater::removeMissingFilesFromDatabase(const QList<MissingFile> &files, const QStringList &paths)
{
    if (files.isEmpty())
        return true;
    setPaths(paths);
    if (!sourcesAvailable() || cancelled())
        return false;

    emit stateChanged(tr("Removing missing files from database..."));
    QList<MissingFile> stillMissing;
    for (const auto &file : files) {
        if (cancelled())
            return false;
        const bool missingPair = file.path.endsWith(".cdg", Qt::CaseInsensitive) &&
                                 findMatchingAudioFile(file.path).isEmpty();
        if (!QFileInfo::exists(file.path) || missingPair)
            stillMissing.append(file);
    }
    if (stillMissing.isEmpty())
        return true;
    if (!sourcesAvailable() || cancelled())
        return false;

    if (!m_db.transaction())
        return reportSqlError(m_db.lastError());
    auto rollback = qScopeGuard([this] { m_db.rollback(); });
    QSqlQuery query(m_db);
    if (!query.prepare("DELETE FROM dbSongs WHERE songid = :id AND path = :path"))
        return reportSqlError(query.lastError());
    bool removed = false;
    int processed = 0;
    for (const auto &file : stillMissing) {
        if (cancelled())
            return false;
        if (progressDue())
            emit progressChanged(processed, stillMissing.size());
        ++processed;
        query.bindValue(":id", file.id);
        query.bindValue(":path", file.path);
        if (!query.exec())
            return reportSqlError(query.lastError());
        removed |= query.numRowsAffected() > 0;
    }
    if (removed && (!query.exec("DELETE FROM queueSongs WHERE song NOT IN (SELECT songid FROM dbSongs)") ||
                    !query.exec("DELETE FROM regularSongs WHERE songid NOT IN (SELECT songid FROM dbSongs)")))
        return reportSqlError(query.lastError());
    if (cancelled())
        return false;
    if (!m_db.commit())
        return reportSqlError(m_db.lastError());
    rollback.dismiss();
    return true;
}

bool DbUpdater::reportSqlError(const QSqlError &error)
{
    m_errors.append(error.text());
    return false;
}

// Finds all potential supported karaoke files in a given directory
bool DbUpdater::DiskEnumerator::findKaraokeFilesOnDisk() {

    emit m_parent.progressChanged(0, 0);

    // cdg and zip files
    QStringList karaoke_files;
    // audio files to match with cdg files
    QStringList audio_files;
    karaoke_files.reserve(200000);
    audio_files.reserve(200000);

    foreach(auto path, m_parent.m_paths ) {
        emit m_parent.stateChanged("Finding karaoke files in " + path);

        int foundInPath = 0;
        QDir dir(path);
        for (const auto &entry : okj::listEntriesRecursively(dir.absolutePath())) {
            if (m_parent.cancelled())
                return false;
            if (entry.isFile()) {
                const std::string ext = entry.suffix().toLower().toStdString();

                if (std::binary_search(m_parent.karaoke_file_extensions.begin(), m_parent.karaoke_file_extensions.end(), ext)) {
                    karaoke_files.append(entry.filePath());
                    foundInPath++;
                }
                else if (std::binary_search(m_parent.audio_file_extensions.begin(), m_parent.audio_file_extensions.end(), ext)) {
                    const QString filePath = entry.filePath();
                    audio_files.append(filePath.left(filePath.lastIndexOf('.')));
                }
            }
            if (m_parent.progressDue()) {
                emit m_parent.stateChanged(QString("Scanning %1\n    %2 found, %3 total...")
                                  .arg(path)
                                  .arg(foundInPath)
                                  .arg(karaoke_files.length()));
            }
        }
    }

    emit m_parent.stateChanged("Sorting...");

    karaoke_files.sort();
    audio_files.sort();

    emit m_parent.stateChanged("Done searching for files.");

    m_karaokeFilesOnDisk = std::move(karaoke_files);
    m_audioFilesOnDisk = std::move(audio_files);
    return !m_parent.cancelled();
}

void DbUpdater::DiskEnumerator::readNextDiskFile()
{
    bool invalid_file_found;
    do {
        m_i_kar++;
        CurrentFile = m_i_kar < m_karaokeFilesOnDisk.size() ? m_karaokeFilesOnDisk.at(m_i_kar) : nullptr;

        if (CurrentFile != nullptr && CurrentFile.endsWith(".cdg", Qt::CaseInsensitive)) {

            // File type is "cdg" and is only valid if there is an audio file with the same filename.
            // Look for an entry with the same filename in the list of audio files.
            invalid_file_found = true;
            const QStringView disk_path_without_ext = QStringView(CurrentFile).left(CurrentFile.length() - 4);

            while (m_i_aud < m_audioFilesOnDisk.size()) {
                int comp_result_audio = disk_path_without_ext.compare(QStringView(m_audioFilesOnDisk.at(m_i_aud)));
                if (comp_result_audio == 0) {
                    // match found!
                    invalid_file_found = false;
                    m_i_aud++;
                    break;
                }
                if (comp_result_audio < 0) {
                    // no match found...
                    break;
                }

                // keep looking - advance to the next audio file in the list
                m_i_aud++;
            }
        }
        else {
            invalid_file_found = false;
        }

    } while (invalid_file_found && !m_parent.cancelled());
    IsValid = CurrentFile != nullptr;
}

bool DbUpdater::DbEnumerator::prepareQuery(bool limitToPaths)
{
    m_dbSongs.setForwardOnly(true);
    if (!limitToPaths) {
        m_dbSongs.prepare("SELECT songid, path, CASE discid WHEN '!!DROPPED!!' THEN 1 ELSE 0 END FROM dbsongs ORDER BY path");
    }
    else {
        QStringList sql_path_filter;
        for(int i = 0; i < m_parent.m_paths.size(); i++) {
            // Each normalized prefix ends in '/'. The half-open BINARY range
            // [prefix, prefix-with-final-slash-replaced-by-'0') matches exactly
            // its descendants, treats all filename characters literally, and
            // can use idx_path. Case matches the scanner's QString comparison.
            sql_path_filter.append(QString("(path COLLATE BINARY >= :pathstart%1 AND path COLLATE BINARY < :pathend%1)").arg(i));
        }

        m_dbSongs.prepare("SELECT songid, path, CASE discid WHEN '!!DROPPED!!' THEN 1 ELSE 0 END FROM dbsongs WHERE " + sql_path_filter.join(" OR ") + " ORDER BY path");
        for(int i = 0; i < m_parent.m_paths.size(); i++) {
            const auto &prefix = m_parent.m_paths[i];
            m_dbSongs.bindValue(QString(":pathstart%1").arg(i), prefix);
            m_dbSongs.bindValue(QString(":pathend%1").arg(i), prefix.chopped(1) + "0");
        }
    }
    if (!m_dbSongs.exec()) {
        m_parent.m_errors.append(m_dbSongs.lastError().text());
        return false;
    }

    return true;
}

void DbUpdater::DbEnumerator::readNextRecord()
{
    if ((IsValid = m_dbSongs.next())) {
        CurrentRecord = DbSongRecord {
            .id =        m_dbSongs.value(0).toInt(),
            .isDropped = m_dbSongs.value(2).toBool(),
            .path =      m_dbSongs.value(1).toString()
        };
    } else if (m_dbSongs.lastError().isValid()) {
        m_parent.reportSqlError(m_dbSongs.lastError());
    }
}

void DbUpdater::setPaths(const QList<QString> &paths)
{
    // Normalize paths:
    //  * Ensure ending separator
    //  * Remove any subpath as parent paths are scanned recursively
    m_paths = QStringList();
    foreach(auto path, paths) {
        path = QDir(QDir::fromNativeSeparators(path)).absolutePath();
        m_paths << (path.endsWith("/") ? path : path + "/");
    }

    m_paths.sort();
    m_paths.removeDuplicates();

    int i=1;
    while (i < m_paths.length()) {
        if (m_paths[i].startsWith(m_paths[i-1]) && m_paths[i].length() > m_paths[i-1].length()) {
            m_paths.removeAt(i);
        }
        else {
            i++;
        }
    }
}

// Match moved files by filename while keeping their database IDs.
bool DbUpdater::fixMissingFiles(QVector<DbSongRecord> &missing, const QStringList &newFiles, QSet<QString> &moved)
{
    QList<DbSongRecord> unmatched;
    QHash<QString, QString> newPaths;
    for (const auto &path : newFiles) {
        if (cancelled())
            return false;
        const auto filename = QFileInfo(path).fileName().toCaseFolded();
        if (!newPaths.contains(filename))
            newPaths.insert(filename, path);
    }

    QSqlQuery query(m_db);
    if (!query.prepare("UPDATE OR IGNORE dbsongs SET path = :newpath WHERE songid = :id AND path = :oldpath"))
        return reportSqlError(query.lastError());
    for (const auto &file : missing) {
        if (cancelled())
            return false;
        const auto path = newPaths.value(QFileInfo(file.path).fileName().toCaseFolded());
        if (!path.isEmpty() && !moved.contains(path)) {
            query.bindValue(":newpath", path);
            query.bindValue(":id", file.id);
            query.bindValue(":oldpath", file.path);
            if (!query.exec())
                return reportSqlError(query.lastError());
            if (query.numRowsAffected() > 0) {
                moved.insert(path);
                continue;
            }
        }
        unmatched.append(file);
    }
    missing = std::move(unmatched);
    return true;
}

bool DbUpdater::progressDue()
{
    if (!m_guiUpdateTimer.isValid())
        m_guiUpdateTimer.start();
    if (m_guiUpdateTimer.elapsed() < 200)
        return false;
    m_guiUpdateTimer.restart();
    return true;
}

// Returns a list of errors encountered while processing
QStringList DbUpdater::getErrors() {
    return m_errors;
}

