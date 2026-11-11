#ifndef DATABASESCHEMA_H
#define DATABASESCHEMA_H

#include <QString>

class QSqlDatabase;

namespace okj {
// Returns an empty string on success or the database error to report at startup.
QString openAndMigrateDatabase(QSqlDatabase &database, const QString &path);
}

#endif
