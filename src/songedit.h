#ifndef OPENKJ_SONGEDIT_H
#define OPENKJ_SONGEDIT_H

#include <QString>

class QSqlDatabase;

namespace okj {

struct SongEdit {
    QString artist;
    QString title;
    QString discId;
    QString filenameBase; // Empty for a metadata-only edit.
};

// The file operation returns an empty string on success or an error description.
using SongFileRename = QString (*)(const QString &source, const QString &destination);

QString saveSongEdit(QSqlDatabase &database, int songId, const QString &oldPath,
                     const SongEdit &edit, SongFileRename renameFile = nullptr);

} // namespace okj

#endif
