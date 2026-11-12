#ifndef DATABASECLEANUP_H
#define DATABASECLEANUP_H

#include <QString>

class QSqlDatabase;

namespace okj {
// Return an empty string after commit, or an error describing the failed operation.
QString clearKaraokeLibrary(QSqlDatabase &database);
QString deleteBreakPlaylist(QSqlDatabase &database, int playlistId);
}

#endif
