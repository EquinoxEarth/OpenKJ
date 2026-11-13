#include "databasecleanup.h"
#include "trackgaincache.h"

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace {
QString cleanupTransaction(QSqlDatabase &database, const auto &operation)
{
    if (!database.transaction())
        return database.lastError().text();

    QString error = operation();
    if (error.isEmpty()) {
        if (database.commit())
            return {};
        error = database.lastError().text();
    }
    if (!database.rollback())
        error += QCoreApplication::translate("DatabaseCleanup", "\nRollback failed: %1")
                     .arg(database.lastError().text());
    return error;
}
}

okj::KaraokeLibraryClearResult okj::clearKaraokeLibrary(QSqlDatabase &database)
{
    const auto error = cleanupTransaction(database, [&] {
        QSqlQuery query(database);
        if (!query.exec("DELETE FROM queueSongs") ||
            !query.exec("DELETE FROM regularSongs") ||
            !query.exec("DELETE FROM rotationSingers") ||
            !query.exec("DELETE FROM regularSingers") ||
            !query.exec("DELETE FROM dbSongs"))
            return query.lastError().text();
        return QString{};
    });
    if (!error.isEmpty())
        return {error, {}};
    return {{}, clearTrackGainCache(database.databaseName())};
}

QString okj::deleteBreakPlaylist(QSqlDatabase &database, int playlistId)
{
    return cleanupTransaction(database, [&] {
        QSqlQuery query(database);
        if (!query.prepare("DELETE FROM bmplsongs WHERE playlist = :id"))
            return query.lastError().text();
        query.bindValue(":id", playlistId);
        if (!query.exec() || !query.prepare("DELETE FROM bmplaylists WHERE playlistid = :id"))
            return query.lastError().text();
        query.bindValue(":id", playlistId);
        if (!query.exec())
            return query.lastError().text();
        if (query.numRowsAffected() != 1)
            return QCoreApplication::translate("DatabaseCleanup", "The selected playlist no longer exists.");
        if (!query.exec("INSERT INTO bmplaylists (title) SELECT 'Default' "
                        "WHERE NOT EXISTS (SELECT 1 FROM bmplaylists)"))
            return query.lastError().text();
        return QString{};
    });
}
