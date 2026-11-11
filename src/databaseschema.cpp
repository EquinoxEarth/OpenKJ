#include "databaseschema.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

QString okj::openAndMigrateDatabase(QSqlDatabase &database, const QString &path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return QStringLiteral("Could not create the database directory.");

    database.setDatabaseName(path);
    if (!database.open())
        return database.lastError().text();

    // SQLite's synchronous setting must be set before starting the transaction.
    QSqlQuery query(database);
    if (!query.exec("PRAGMA synchronous=FULL") ||
        !query.exec("PRAGMA cache_size=300000") ||
        !query.exec("PRAGMA temp_store=2"))
        return query.lastError().text();

    if (!database.transaction())
        return database.lastError().text();
    auto rollback = qScopeGuard([&database, &query] {
        query.finish();
        if (!database.rollback())
            qWarning() << "Database migration rollback failed:" << database.lastError().text();
    });

    if (!query.exec("PRAGMA user_version") || !query.next())
        return QStringLiteral("Could not read the database schema version. %1").arg(query.lastError().text());
    const int version = query.value(0).toInt();
    query.finish();

    if (!query.exec("CREATE TABLE IF NOT EXISTS dbSongs (songid INTEGER PRIMARY KEY AUTOINCREMENT, artist COLLATE NOCASE, title COLLATE NOCASE, discid COLLATE NOCASE, duration INTEGER, path VARCHAR(700) NOT NULL UNIQUE, filename COLLATE NOCASE)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS rotationSingers (singerid INTEGER PRIMARY KEY AUTOINCREMENT, name COLLATE NOCASE UNIQUE, position INTEGER NOT NULL, regular LOGICAL DEFAULT(0), regularid INTEGER)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS queueSongs (qsongid INTEGER PRIMARY KEY AUTOINCREMENT, singer INT, song INTEGER NOT NULL, artist INT, title INT, discid INT, path INT, keychg INT, played LOGICAL DEFAULT(0), position INT)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS regularSingers (regsingerid INTEGER PRIMARY KEY AUTOINCREMENT, name COLLATE NOCASE UNIQUE, ph1 INT, ph2 INT, ph3 INT)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS regularSongs (regsongid INTEGER PRIMARY KEY AUTOINCREMENT, regsingerid INTEGER NOT NULL, songid INTEGER NOT NULL, keychg INTEGER, position INTEGER)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS sourceDirs (path VARCHAR(255) UNIQUE, pattern INTEGER)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS bmsongs (songid INTEGER PRIMARY KEY AUTOINCREMENT, artist COLLATE NOCASE, title COLLATE NOCASE, path VARCHAR(700) NOT NULL UNIQUE, filename COLLATE NOCASE, duration TEXT, searchstring TEXT)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS bmplaylists (playlistid INTEGER PRIMARY KEY AUTOINCREMENT, title COLLATE NOCASE NOT NULL UNIQUE)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS bmplsongs (plsongid INTEGER PRIMARY KEY AUTOINCREMENT, playlist INT, position INT, artist INT, title INT, filename INT, duration INT, path INT)") ||
        !query.exec("CREATE TABLE IF NOT EXISTS bmsrcdirs (path NOT NULL)"))
        return query.lastError().text();

    // Older releases could leave a column behind without advancing user_version.
    const auto addColumn = [&query](const QString &table, const QString &column, const QString &type) {
        if (!query.exec(QStringLiteral("SELECT * FROM %1 LIMIT 0").arg(table)))
            return false;
        const bool exists = query.record().indexOf(column) >= 0;
        query.finish();
        return exists || query.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, type));
    };

    if (version < 100 && !addColumn("sourceDirs", "custompattern", "INTEGER"))
        return query.lastError().text();
    if (version < 101 &&
        !query.exec("CREATE TABLE IF NOT EXISTS custompatterns (patternid INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, artistregex TEXT, artistcapturegrp INT, titleregex TEXT, titlecapturegrp INT, discidregex TEXT, discidcapturegrp INT)"))
        return query.lastError().text();
    if (version < 102 && !query.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_path ON dbsongs(path)"))
        return query.lastError().text();
    if (version < 103 &&
        (!addColumn("dbsongs", "searchstring", "TEXT") ||
         !query.exec("UPDATE dbsongs SET searchstring = filename || ' ' || artist || ' ' || title || ' ' || discid")))
        return query.lastError().text();
    if (version < 105 && !addColumn("rotationSingers", "addts", "TIMESTAMP"))
        return query.lastError().text();

    constexpr int currentVersion = 106;
    if (version < currentVersion) {
        if (!query.exec("CREATE TABLE IF NOT EXISTS dbSongHistory (id INTEGER PRIMARY KEY AUTOINCREMENT, filepath TEXT, artist TEXT, title TEXT, songid TEXT, timestamp TIMESTAMP)") ||
            !query.exec("CREATE INDEX IF NOT EXISTS idx_filepath ON dbSongHistory(filepath)") ||
            !addColumn("dbsongs", "plays", "INT DEFAULT(0)") ||
            !addColumn("dbsongs", "lastplay", "TIMESTAMP") ||
            !query.exec("CREATE TABLE IF NOT EXISTS historySingers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE)") ||
            !query.exec("CREATE TABLE IF NOT EXISTS historySongs (id INTEGER PRIMARY KEY AUTOINCREMENT, historySinger INT NOT NULL, filepath TEXT NOT NULL, artist TEXT, title TEXT, songid TEXT, keychange INT DEFAULT(0), plays INT DEFAULT(0), lastplay TIMESTAMP)") ||
            !query.exec("CREATE INDEX IF NOT EXISTS idx_historySinger ON historySongs(historySinger)"))
            return query.lastError().text();

        // The old importer only created singers with a matching library song.
        if (!query.exec(R"SQL(
            INSERT INTO historySingers (name)
            SELECT DISTINCT s.name
            FROM regularSingers s
            JOIN regularSongs r ON r.regsingerid = s.regsingerid
            JOIN dbsongs d ON d.songid = r.songid
            WHERE COALESCE(d.artist, '') COLLATE BINARY <> '--Dropped Song--'
              AND NOT EXISTS (SELECT 1 FROM historySingers h WHERE h.name = s.name COLLATE BINARY)
        )SQL"))
            return QStringLiteral("Could not import regular singers. %1").arg(query.lastError().text());

        // Match the old importer's play counts and retain the last queued key change.
        // Keep history rows already present from an interrupted older upgrade.
        if (!query.prepare(R"SQL(
            WITH legacy AS (
                SELECT h.id AS singer, d.path, d.artist, d.title, d.discid, r.keychg,
                       COUNT(*) OVER per_song AS plays,
                       ROW_NUMBER() OVER (
                           per_song ORDER BY r.position DESC, r.regsongid DESC
                       ) AS latest
                FROM regularSongs r
                JOIN regularSingers s ON s.regsingerid = r.regsingerid
                JOIN historySingers h ON h.name = s.name COLLATE BINARY
                JOIN dbsongs d ON d.songid = r.songid
                WHERE COALESCE(d.artist, '') COLLATE BINARY <> '--Dropped Song--'
                WINDOW per_song AS (PARTITION BY h.id, d.path)
            )
            INSERT INTO historySongs (historySinger, filepath, artist, title, songid, keychange, plays, lastplay)
            SELECT singer, path, artist, title, discid, COALESCE(keychg, 0), plays, ?
            FROM legacy
            WHERE latest = 1
              AND NOT EXISTS (
                  SELECT 1 FROM historySongs h WHERE h.historySinger = legacy.singer AND h.filepath = legacy.path
              )
        )SQL"))
            return QStringLiteral("Could not prepare the regular-singer history import. %1").arg(query.lastError().text());
        query.addBindValue(QDateTime::currentDateTime());
        if (!query.exec())
            return QStringLiteral("Could not import regular-singer history. %1").arg(query.lastError().text());
    }

    // Catch required schema missing from an older, incorrectly advanced database.
    if (!query.exec("SELECT searchstring, plays, lastplay FROM dbsongs LIMIT 0") ||
        !query.exec("SELECT addts FROM rotationSingers LIMIT 0") ||
        !query.exec("SELECT custompattern FROM sourceDirs LIMIT 0") ||
        !query.exec("SELECT patternid FROM custompatterns LIMIT 0") ||
        !query.exec("SELECT id, name FROM historySingers LIMIT 0") ||
        !query.exec("SELECT id, historySinger, filepath, artist, title, songid, keychange, plays, lastplay FROM historySongs LIMIT 0"))
        return QStringLiteral("The database schema is incomplete. %1").arg(query.lastError().text());
    query.finish();

    if (version < currentVersion &&
        !query.exec(QStringLiteral("PRAGMA user_version = %1").arg(currentVersion)))
        return query.lastError().text();

    if (!database.commit())
        return database.lastError().text();
    rollback.dismiss();
    return {};
}
