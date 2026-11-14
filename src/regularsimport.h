#ifndef REGULARSIMPORT_H
#define REGULARSIMPORT_H

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QStringList>

class QSqlDatabase;

namespace RegularsImport {
struct Song {
    QString path;
    QString artist;
    QString title;
    QString discId;
    int keyChange = 0;
    int plays = 0;
    QDateTime lastPlay;
};

struct Singer {
    QString name;
    QList<Song> songs;
    int existingId = -1;
    bool replace = false;
};

struct Export {
    QList<Singer> singers;
    QMap<QString, QString> roots;
    bool legacy = false;
    QString error;
};

struct Result {
    QString error;
    QStringList warnings;
    int imported = 0;
};

Export readExport(const QString &filename);
Result importSingers(QSqlDatabase &database, QList<Singer> singers,
                     const QMap<QString, QString> &mappings, bool legacy);
}

#endif
