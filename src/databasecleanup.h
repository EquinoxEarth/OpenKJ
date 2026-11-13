#ifndef DATABASECLEANUP_H
#define DATABASECLEANUP_H

#include <QString>

class QSqlDatabase;

namespace okj {
struct KaraokeLibraryClearResult {
    QString error;
    QString cacheError;
};
KaraokeLibraryClearResult clearKaraokeLibrary(QSqlDatabase &database);
QString deleteBreakPlaylist(QSqlDatabase &database, int playlistId);
}

#endif
