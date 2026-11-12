#include "songedit.h"
#include "okjutil.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

namespace {

struct FileMove {
    QString source;
    QString destination;
};

QString renameSongFile(const QString &source, const QString &destination)
{
    QFile file(source);
    return file.rename(destination) ? QString() : file.errorString();
}

QString pathWithDirectoryEntrySpelling(const QFileInfo &file)
{
    const auto path = file.absoluteFilePath();
    if (!file.exists())
        return path;

    // On case-insensitive volumes the audio lookup can find "track.MP3"
    // through "track.mp3". Read the stored spelling without following links.
    const auto directory = file.absoluteDir();
    const auto name = file.fileName();
    const auto entries = directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                 QDir::NoDotAndDotDot,
                                             QDir::NoSort);
    if (entries.contains(name))
        return path;

    QString matchingName;
    for (const auto &entry : entries) {
        if (entry.compare(name, Qt::CaseInsensitive) != 0)
            continue;
        if (!matchingName.isEmpty())
            return path;
        matchingName = entry;
    }
    return matchingName.isEmpty() ? path : directory.filePath(matchingName);
}

} // namespace

QString okj::saveSongEdit(QSqlDatabase &database, int songId, const QString &oldPath,
                          const SongEdit &edit, SongFileRename renameFile)
{
    const auto tr = [](const char *message) { return QCoreApplication::translate("MainWindow", message); };
    if (!renameFile)
        renameFile = renameSongFile;

    QList<FileMove> moves;
    QString newPath = oldPath;
    QString pairedAudioDestination;
    if (!edit.filenameBase.isEmpty()) {
        if (edit.filenameBase.contains(u'/') || edit.filenameBase.contains(u'\\') ||
            edit.filenameBase.contains(QChar::Null) || edit.filenameBase == "." || edit.filenameBase == "..")
            return tr("The new filename must be within the song's folder.");

        const auto planMove = [&](const QString &path) {
            const QFileInfo source(pathWithDirectoryEntrySpelling(QFileInfo(path)));
            const QString suffix = source.suffix();
            const QString filename = suffix.isEmpty() ? edit.filenameBase : edit.filenameBase + u'.' + suffix;
            return FileMove{source.absoluteFilePath(), source.absoluteDir().filePath(filename)};
        };
        const auto song = planMove(oldPath);
        if (song.source != song.destination) {
            newPath = song.destination;
            moves.append(song);
        }
        if (QFileInfo(oldPath).suffix().compare("cdg", Qt::CaseInsensitive) == 0) {
            const auto audioPath = findMatchingAudioFile(oldPath);
            if (audioPath.isEmpty())
                return tr("The CDG file's matching audio file is missing.");
            const auto audio = planMove(audioPath);
            pairedAudioDestination = audio.destination;
            if (audio.source != audio.destination)
                moves.append(audio);
        }

        for (const auto &move : moves) {
            const QFileInfo source(move.source);
            const QFileInfo destination(move.destination);
            if (!source.isFile() || !source.isWritable())
                return tr("The source file is missing or unwritable.\n%1").arg(move.source);
            // A case-only rename can resolve to the same directory entry.
            if (destination.isSymLink() ||
                (destination.exists() && pathWithDirectoryEntrySpelling(destination) != move.source))
                return tr("A file already exists at the destination.\n%1").arg(move.destination);
        }
    }

    if (!database.transaction())
        return database.lastError().text();

    qsizetype moved = 0;
    const auto fail = [&](const QString &reason) {
        QStringList errors{reason};
        while (moved > 0) {
            const auto &move = moves[--moved];
            const auto error = renameFile(move.destination, move.source);
            if (!error.isEmpty())
                errors.append(tr("Could not restore the original file.\nCurrent location: %1\nOriginal location: %2\n%3")
                                  .arg(move.destination, move.source, error));
        }
        if (!database.rollback())
            errors.append(tr("Could not roll back the database edit.\n%1").arg(database.lastError().text()));
        return errors.join(QStringLiteral("\n\n"));
    };

    // Stage the update first to validate the current row and acquire the SQLite write lock.
    const bool pathChanged = newPath != oldPath;
    QSqlQuery query(database);
    if (!query.prepare(pathChanged
                           ? "UPDATE dbsongs SET artist = ?, title = ?, discid = ?, searchstring = ?, path = ?, filename = ? WHERE songid = ? AND path = ?"
                           : "UPDATE dbsongs SET artist = ?, title = ?, discid = ?, searchstring = ? WHERE songid = ? AND path = ?"))
        return fail(query.lastError().text());
    const QFileInfo newFile(newPath);
    query.addBindValue(edit.artist);
    query.addBindValue(edit.title);
    query.addBindValue(edit.discId);
    query.addBindValue(newFile.completeBaseName() + u' ' + edit.artist + u' ' + edit.title + u' ' + edit.discId);
    if (pathChanged) {
        query.addBindValue(newPath);
        query.addBindValue(newFile.fileName());
    }
    query.addBindValue(songId);
    query.addBindValue(oldPath);
    if (!query.exec())
        return fail(query.lastError().text());
    if (query.numRowsAffected() != 1)
        return fail(tr("The library entry changed while the edit was open. Reopen the song and try again."));

    if (pathChanged) {
        if (!query.prepare("UPDATE historySongs SET filepath = ? WHERE filepath = ?"))
            return fail(query.lastError().text());
        query.addBindValue(newPath);
        query.addBindValue(oldPath);
        if (!query.exec())
            return fail(query.lastError().text());
    }
    query.finish();

    for (const auto &move : moves) {
        const auto error = renameFile(move.source, move.destination);
        if (!error.isEmpty())
            return fail(tr("Could not rename the file.\nFrom: %1\nTo: %2\n%3")
                            .arg(move.source, move.destination, error));
        ++moved;
    }

    if (!pairedAudioDestination.isEmpty()) {
        const auto actualAudio = findMatchingAudioFile(newPath);
        if (actualAudio.isEmpty() ||
            QFileInfo(pathWithDirectoryEntrySpelling(QFileInfo(actualAudio))).canonicalFilePath() !=
                QFileInfo(pairedAudioDestination).canonicalFilePath())
            return fail(tr("The renamed CDG would use a different audio file.\n%1").arg(actualAudio));
    }

    if (!database.commit())
        return fail(database.lastError().text());
    return {};
}
