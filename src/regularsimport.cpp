#include "regularsimport.h"
#include "regularpathmapping.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QXmlStreamReader>

namespace {
using namespace RegularsImport;

QString tr(const char *message)
{
    return QCoreApplication::translate("RegularsImport", message);
}

QString matchKey(const QString &artist, const QString &title, const QString &discId = {})
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{discId.toCaseFolded(),
        artist.toCaseFolded(), title.toCaseFolded()}).toJson(QJsonDocument::Compact));
}

QString readLegacy(QFile &file, QList<Singer> &singers)
{
    QXmlStreamReader xml(&file);
    bool inSinger = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.name() == QLatin1String("singer")) {
            if (xml.isEndElement()) {
                inSinger = false;
            } else if (xml.isStartElement()) {
                if (inSinger)
                    return tr("The export contains nested singers.");
                singers.append(Singer{xml.attributes().value("name").toString(), {}});
                inSinger = true;
            }
        } else if (inSinger && xml.isStartElement() && xml.name() == QLatin1String("song")) {
            const auto attributes = xml.attributes();
            singers.last().songs.append(Song{{}, attributes.value("artist").toString(),
                attributes.value("title").toString(), attributes.value("discid").toString(),
                attributes.value("key").toInt(), 1, {}});
        }
    }
    return xml.errorString();
}

QString readJson(QFile &file, Export &result)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (file.error() != QFile::NoError)
        return file.errorString();
    if (error.error != QJsonParseError::NoError)
        return error.errorString();

    QJsonArray singers = document.array();
    if (document.isObject()) {
        const auto object = document.object();
        if (object.value("formatVersion").toDouble() != 2 || !object.value("roots").isObject() ||
            !object.value("singers").isArray())
            return tr("Unsupported regular-singer export format.");
        singers = object.value("singers").toArray();
        const auto roots = object.value("roots").toObject();
        const QRegularExpression rootName("^[A-Za-z0-9_]+$");
        for (auto it = roots.begin(); it != roots.end(); ++it) {
            const QString path = it.value().toString();
            if (!rootName.match(it.key()).hasMatch() || !RegularPaths::absolute(RegularPaths::normalized(path)))
                return tr("The export contains an invalid root folder.");
            result.roots.insert('%' + it.key() + '%', path);
        }
    }

    const QRegularExpression rootToken("^(%[A-Za-z0-9_]+%)/");
    for (const auto &value : singers) {
        const auto object = value.toObject();
        Singer singer{object.value("name").toString(), {}};
        if (!object.value("songs").isArray())
            return tr("The export contains an invalid singer.");
        for (const auto &entry : object.value("songs").toArray()) {
            const auto song = entry.toObject();
            const QString path = song.value("filepath").toString();
            if (path.startsWith('%')) {
                const auto token = rootToken.match(path);
                if (!token.hasMatch() || !result.roots.contains(token.captured(1)))
                    return tr("A song refers to an unknown root folder.");
            } else if (!RegularPaths::absolute(RegularPaths::normalized(path))) {
                return tr("A song has an invalid file path.");
            }
            const QString date = song.value("lastplay").toString();
            auto lastPlay = QDateTime::fromString(date, Qt::ISODateWithMs);
            if (!lastPlay.isValid())
                lastPlay = QDateTime::fromString(date, Qt::TextDate);
            singer.songs.append(Song{path, song.value("artist").toString(), song.value("title").toString(),
                song.value("songid").toString(), song.value("keychange").toInt(), song.value("plays").toInt(),
                lastPlay});
        }
        result.singers.append(std::move(singer));
    }
    return {};
}

QString matchSongs(QSqlDatabase &database, QList<Singer> &singers,
                   const QMap<QString, QString> &mappings, bool legacy, QStringList &warnings)
{
    QSqlQuery query(database);
    using Matches = QHash<QString, QStringList>;
    Matches exact, byTitle, byDisc;
    QHash<QString, QString> titleDiscIds;
    QHash<QString, bool> availablePaths;
    const auto isAvailable = [&availablePaths](const QString &path) {
        auto found = availablePaths.constFind(path);
        if (found != availablePaths.cend())
            return found.value();
        const bool available = QFileInfo(path).isFile();
        availablePaths.insert(path, available);
        return available;
    };
    const auto findMatches = [&isAvailable](const Matches &index, const QString &key) {
        auto paths = index.value(key);
        paths.removeIf([&isAvailable](const QString &path) { return !isAvailable(path); });
        return paths;
    };
    for (auto &singer : singers) {
        for (auto &song : singer.songs) {
            if (!legacy) {
                song.path = RegularPaths::resolve(song.path, mappings);
                if (song.path.isEmpty())
                    return tr("A song path escapes its root folder or cannot be mapped.");
                if (isAvailable(song.path))
                    continue;
            }
            if (!song.discId.isEmpty() && !song.artist.isEmpty() && !song.title.isEmpty())
                exact.insert(matchKey(song.artist, song.title, song.discId), {});
            if (legacy && !song.discId.isEmpty()) {
                byDisc.insert(song.discId.toCaseFolded(), {});
                byTitle.insert(matchKey(song.artist, song.title), {});
            }
        }
    }

    // Scan the library once and retain candidates only for the selected export.
    if (!exact.isEmpty() || !byDisc.isEmpty()) {
        if (!query.exec("SELECT discid, artist, title, path FROM dbsongs"))
            return query.lastError().text();
        const auto retain = [](Matches &index, const QString &key, const QString &value) {
            auto it = index.find(key);
            if (it != index.end())
                it->append(value);
        };
        while (query.next()) {
            const QString discId = query.value(0).toString();
            const QString artist = query.value(1).toString();
            const QString title = query.value(2).toString();
            const QString path = query.value(3).toString();
            retain(exact, matchKey(artist, title, discId), path);
            if (legacy) {
                retain(byDisc, discId.toCaseFolded(), path);
                // The legacy vendor fallback searches within this artist/title group.
                const QString key = matchKey(artist, title);
                auto matches = byTitle.find(key);
                if (matches != byTitle.end()) {
                    matches->append(path);
                    titleDiscIds.insert(path, discId);
                }
            }
        }
        if (query.lastError().isValid())
            return query.lastError().text();
        query.finish();
    }

    for (auto &singer : singers) {
        for (auto &song : singer.songs) {
            if (!legacy && isAvailable(song.path))
                continue;
            auto available = findMatches(exact, matchKey(song.artist, song.title, song.discId));
            if (legacy) {
                if (available.isEmpty() && !song.discId.isEmpty()) {
                    qsizetype vendorLength = 0;
                    while (vendorLength < song.discId.size() && song.discId.at(vendorLength).isLetter())
                        ++vendorLength;
                    if (vendorLength > 0) {
                        const QString vendor = song.discId.left(vendorLength);
                        const auto paths = byTitle.value(matchKey(song.artist, song.title));
                        for (const auto &path : paths) {
                            if (titleDiscIds.value(path).contains(vendor, Qt::CaseInsensitive) && isAvailable(path))
                                available.append(path);
                        }
                    }
                    if (available.isEmpty())
                        available = findMatches(byDisc, song.discId.toCaseFolded());
                }
                song.lastPlay = QDateTime::currentDateTime();
            }
            if (available.size() == 1) {
                song.path = available.first();
            } else {
                const QString problem = available.isEmpty() ? tr("File unavailable") : tr("Multiple library matches");
                warnings.append(tr("%1 / %2 / %3: %4").arg(singer.name, song.discId, song.title, problem));
            }
        }
        // The legacy file has no original path to retain for unresolved songs.
        if (legacy)
            singer.songs.removeIf([](const Song &song) { return song.path.isEmpty(); });
    }
    if (legacy)
        singers.removeIf([](const Singer &singer) { return singer.songs.isEmpty(); });
    return {};
}

QString writeSingers(QSqlDatabase &database, const QList<Singer> &singers, bool legacy)
{
    if (!database.transaction())
        return database.lastError().text();
    auto rollback = qScopeGuard([&database] {
        if (!database.rollback())
            qWarning() << "Regular-singer import rollback failed:" << database.lastError().text();
    });
    QSqlQuery lookup(database), add(database), clear(database), paths(database), insert(database), update(database);
    if (!lookup.exec("SELECT id, name FROM historySingers"))
        return lookup.lastError().text();
    QHash<QString, int> ids;
    QSet<QString> ambiguous;
    while (lookup.next()) {
        const QString key = lookup.value(1).toString().toCaseFolded();
        if (ids.contains(key))
            ambiguous.insert(key);
        ids.insert(key, lookup.value(0).toInt());
    }
    if (lookup.lastError().isValid())
        return lookup.lastError().text();
    lookup.finish();

    if (!add.prepare("INSERT INTO historySingers (name) VALUES (?)"))
        return add.lastError().text();
    if (!clear.prepare("DELETE FROM historySongs WHERE historySinger = ?"))
        return clear.lastError().text();
    if (!paths.prepare("SELECT filepath FROM historySongs WHERE historySinger = ?"))
        return paths.lastError().text();
    if (!insert.prepare("INSERT INTO historySongs (historySinger, filepath, artist, title, songid, keychange, plays, lastplay) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)"))
        return insert.lastError().text();
    if (legacy && !update.prepare("UPDATE historySongs SET artist = ?, title = ?, songid = ?, keychange = ?, "
                                  "plays = plays + 1, lastplay = ? WHERE historySinger = ? AND filepath = ?"))
        return update.lastError().text();

    for (const auto &singer : singers) {
        const QString key = singer.name.toCaseFolded();
        int id = ids.value(key, -1);
        if (ambiguous.contains(key) || id != singer.existingId)
            return tr("The existing singer named %1 changed or is ambiguous. Review the import and try again.").arg(singer.name);
        QSet<QString> existingPaths;
        if (id == -1) {
            add.bindValue(0, singer.name);
            if (!add.exec())
                return add.lastError().text();
            id = add.lastInsertId().toInt();
        } else if (singer.replace) {
            clear.bindValue(0, id);
            if (!clear.exec())
                return clear.lastError().text();
        } else {
            paths.bindValue(0, id);
            if (!paths.exec())
                return paths.lastError().text();
            while (paths.next())
                existingPaths.insert(paths.value(0).toString());
            if (paths.lastError().isValid())
                return paths.lastError().text();
            paths.finish();
        }

        for (const auto &song : singer.songs) {
            if (existingPaths.contains(song.path)) {
                if (legacy) {
                    update.bindValue(0, song.artist);
                    update.bindValue(1, song.title);
                    update.bindValue(2, song.discId);
                    update.bindValue(3, song.keyChange);
                    update.bindValue(4, song.lastPlay);
                    update.bindValue(5, id);
                    update.bindValue(6, song.path);
                    if (!update.exec())
                        return update.lastError().text();
                }
                continue;
            }
            insert.bindValue(0, id);
            insert.bindValue(1, song.path);
            insert.bindValue(2, song.artist);
            insert.bindValue(3, song.title);
            insert.bindValue(4, song.discId);
            insert.bindValue(5, song.keyChange);
            insert.bindValue(6, song.plays);
            insert.bindValue(7, song.lastPlay);
            if (!insert.exec())
                return insert.lastError().text();
            existingPaths.insert(song.path);
        }
    }
    if (!database.commit())
        return database.lastError().text();
    rollback.dismiss();
    return {};
}
}

RegularsImport::Export RegularsImport::readExport(const QString &filename)
{
    Export result;
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    result.legacy = filename.endsWith(".xml", Qt::CaseInsensitive);
    result.error = result.legacy ? readLegacy(file, result.singers) : readJson(file, result);
    if (!result.error.isEmpty())
        return result;

    QSet<QString> names;
    for (const auto &singer : result.singers) {
        const QString key = singer.name.toCaseFolded();
        if (key.trimmed().isEmpty() || names.contains(key)) {
            result.error = tr("The export contains an invalid or duplicate singer.");
            break;
        }
        names.insert(key);
    }
    return result;
}

RegularsImport::Result RegularsImport::importSingers(QSqlDatabase &database, QList<Singer> singers,
                                                      const QMap<QString, QString> &mappings, bool legacy)
{
    Result result;
    result.error = matchSongs(database, singers, mappings, legacy, result.warnings);
    if (result.error.isEmpty() && !singers.isEmpty()) {
        result.error = writeSingers(database, singers, legacy);
        if (result.error.isEmpty())
            result.imported = singers.size();
    }
    return result;
}
