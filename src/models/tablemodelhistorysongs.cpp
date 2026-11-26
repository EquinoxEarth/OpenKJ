

#include "tablemodelhistorysongs.h"
#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QSqlError>
#include <QFontMetrics>
#include <QSqlQuery>
#include <algorithm>

TableModelHistorySongs::TableModelHistorySongs(TableModelKaraokeSongs &songsModel) : m_karaokeSongsModel(songsModel) {
    m_logger = spdlog::get("logger");
    setFont(m_settings.applicationFont());
}


int TableModelHistorySongs::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_visibleRows.size());
}

int TableModelHistorySongs::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : 9;
}

QVariant TableModelHistorySongs::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rowCount({}))
        return {};
    const auto &song = m_songs[m_visibleRows[index.row()]];
    switch (role) {
        case Qt::FontRole:
            return m_itemFont;
        case Qt::ForegroundRole:
            if (m_karaokeSongsModel.getIdForPath(song.filePath) == -1)
                return QColor(Qt::gray);
            return {};
        case Qt::ToolTipRole:
            if (m_karaokeSongsModel.getIdForPath(song.filePath) == -1)
                return QString("Song doesn't exist in current database.");
            return {};
        case Qt::TextAlignmentRole:
            return getTextAlignment(index);
        case Qt::DisplayRole:
            return getDisplayData(song, index.column());
        case Qt::SizeHintRole:
            return getSizeHint(index.column());
        default:
            return {};
    }
}

QVariant TableModelHistorySongs::getTextAlignment(const QModelIndex &index) {
    switch (index.column()) {
        case KEY_CHANGE:
            return static_cast<int>(Qt::AlignCenter);
        case SUNG_COUNT:
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        case LAST_SUNG:
            return static_cast<int>(Qt::AlignHCenter | Qt::AlignVCenter);
        default:
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
}

QVariant TableModelHistorySongs::getDisplayData(const okj::HistorySong &song, int column) {
    switch (column) {
        case SONG_ID:
            return song.id;
        case SINGER_ID:
            return song.historySinger;
        case PATH:
            return song.filePath;
        case ARTIST:
            return song.artist;
        case TITLE:
            return song.title;
        case SONGID:
            return song.songid;
        case KEY_CHANGE: {
            int key = song.keyChange;
            if (key == 0)
                return {};
            if (key > 0)
                return "+" + QString::number(key);
            else
                return key;
        }
        case SUNG_COUNT:
            return song.plays;
        case LAST_SUNG: {
            QLocale locale;
            return song.lastPlayed.toString(locale.dateFormat(QLocale::ShortFormat));
        }
    }
    return {};
}

void TableModelHistorySongs::loadSinger(const int historySingerId) {
    auto songs = historySingerId < 0 ? std::vector<okj::HistorySong>{} : getSingerSongs(historySingerId);
    beginResetModel();
    m_songs = std::move(songs);
    sortSongs();
    rebuildVisibleRows();
    endResetModel();
}

void TableModelHistorySongs::loadSinger(const QString &historySingerName) {
    if (m_currentSinger != historySingerName)
        m_searchText.clear();
    m_currentSinger = historySingerName;
    loadSinger(getSingerId(historySingerName));
}

void TableModelHistorySongs::search(const QString &text) {
    beginResetModel();
    m_searchText = text;
    rebuildVisibleRows();
    endResetModel();
}

void TableModelHistorySongs::rebuildVisibleRows() {
    const bool ignoreApostrophes = m_settings.ignoreAposInSearch();
    QString query = m_searchText;
    query.replace(u',', u' ');
    query.replace(u'&', QStringLiteral(" and "));
    if (ignoreApostrophes)
        query.replace(u'\'', u' ');
    const auto words = query.simplified().split(u' ', Qt::SkipEmptyParts);

    m_visibleRows.clear();
    m_visibleRows.reserve(m_songs.size());
    for (size_t row = 0; row < m_songs.size(); ++row) {
        if (!words.isEmpty()) {
            const auto &song = m_songs[row];
            QString text = song.artist + u' ' + song.title + u' ' + song.songid + u' ' + QFileInfo(song.filePath).fileName();
            text.replace(u'&', QStringLiteral(" and "));
            if (ignoreApostrophes)
                text.remove(u'\'');
            if (!std::all_of(words.cbegin(), words.cend(), [&](const QString &word) {
                return text.contains(word, Qt::CaseInsensitive);
            }))
                continue;
        }
        m_visibleRows.push_back(row);
    }
}

void TableModelHistorySongs::saveSong(const QString &singerName, const QString &filePath, const QString &artist,
                                      const QString &title, const QString &songid, const int keyChange) {
    if (artist == "--Dropped Song--") {
        m_logger->info("{} Song was added via drag and drop from an external source, not adding to history",
                       m_loggingPrefix);
        return;
    }
    QSqlQuery query;
    auto historySingerId = getSingerId(singerName);
    if (historySingerId != -1 && songExists(historySingerId, filePath)) {
        query.prepare("UPDATE historySongs SET artist = :artist, title = :title, songid = :songid, "
                      "keychange = :keychange, plays = plays + 1, lastplay = :datetime "
                      "WHERE filePath = :filepath AND historysinger = :historysinger");
        query.bindValue(":artist", artist);
        query.bindValue(":title", title);
        query.bindValue(":songid", songid);
        query.bindValue(":keychange", keyChange);
        query.bindValue(":filepath", filePath);
        query.bindValue(":historysinger", historySingerId);
        query.bindValue(":datetime", QDateTime::currentDateTime());
        query.exec();
        if (auto error = query.lastError(); error.type() != QSqlError::NoError)
            m_logger->error("{} DB error: {}", m_loggingPrefix, error.text());
        loadSinger(m_currentSinger);
        return;
    }
    if (historySingerId == -1) {
        historySingerId = addSinger(singerName);
    }
    query.prepare(
            "INSERT INTO historySongs (historySinger, filepath, artist, title, songid, keychange, plays, lastplay) "
            "values (:historySinger, :filepath, :artist, :title, :songid, :keychange, 1, :datetime)");
    query.bindValue(":artist", artist);
    query.bindValue(":title", title);
    query.bindValue(":songid", songid);
    query.bindValue(":keychange", keyChange);
    query.bindValue(":filepath", filePath);
    query.bindValue(":historySinger", historySingerId);
    query.bindValue(":datetime", QDateTime::currentDateTime());
    query.exec();
    if (auto error = query.lastError(); error.type() != QSqlError::NoError)
        m_logger->error("{} DB error: {}", m_loggingPrefix, error.text());
    loadSinger(m_currentSinger);
}

void TableModelHistorySongs::saveSong(const QString &singerName, const QString &filePath, const QString &artist,
                                      const QString &title, const QString &songid, const int keyChange, int plays,
                                      const QDateTime &lastPlayed) {
    QSqlQuery query;
    auto historySingerId = getSingerId(singerName);
    if (historySingerId != -1 && songExists(historySingerId, filePath)) {
        return;
    }
    if (historySingerId == -1) {
        historySingerId = addSinger(singerName);
    }
    query.prepare(
            "INSERT INTO historySongs (historySinger, filepath, artist, title, songid, keychange, plays, lastplay) "
            "values (:historySinger, :filepath, :artist, :title, :songid, :keychange, :plays, :datetime)");
    query.bindValue(":artist", artist);
    query.bindValue(":title", title);
    query.bindValue(":songid", songid);
    query.bindValue(":keychange", keyChange);
    query.bindValue(":filepath", filePath);
    query.bindValue(":historySinger", historySingerId);
    query.bindValue(":plays", plays);
    query.bindValue(":datetime", lastPlayed);
    query.exec();
    if (auto error = query.lastError(); error.type() != QSqlError::NoError)
        m_logger->error("{} DB error: {}", m_loggingPrefix, error.text());
    loadSinger(m_currentSinger);
}

void TableModelHistorySongs::deleteSong(const int historySongId) {
    QSqlQuery query;
    query.prepare("DELETE FROM historySongs WHERE id = :historySongId");
    query.bindValue(":historySongId", historySongId);
    query.exec();
    loadSinger(m_currentSinger);
}

int TableModelHistorySongs::addSinger(const QString &name) const {
    QSqlQuery query;
    query.prepare("INSERT INTO historySingers (name) VALUES( :name )");
    query.bindValue(":name", name);
    query.exec();
    if (auto error = query.lastError(); error.type() != QSqlError::NoError)
        m_logger->error("{} DB error: {}", m_loggingPrefix, error.text());
    return query.lastInsertId().toInt();
}

bool TableModelHistorySongs::songExists(const int historySingerId, const QString &filePath) const {
    QSqlQuery query;
    query.prepare("SELECT id FROM historySongs WHERE historySinger = :historySinger AND filepath = :filePath LIMIT 1");
    query.bindValue(":historySinger", historySingerId);
    query.bindValue(":filePath", filePath);
    query.exec();
    if (query.next())
        return true;
    return false;
}

int TableModelHistorySongs::getSingerId(const QString &name) const {
    int retVal = -1;
    QSqlQuery query;
    query.prepare("SELECT id FROM historySingers WHERE name = :name LIMIT 1");
    query.bindValue(":name", name);
    query.exec();
    if (query.next()) {
        retVal = query.value(0).toInt();
    }
    return retVal;
}

std::vector<okj::HistorySong> TableModelHistorySongs::getSingerSongs(const int historySingerId) {
    std::vector<okj::HistorySong> songs;
    QSqlQuery query;
    query.prepare("SELECT * from historySongs WHERE historySinger = :historySinger");
    query.bindValue(":historySinger", historySingerId);
    query.exec();
    while (query.next()) {
        okj::HistorySong song;
        song.id = query.value(0).toUInt();
        song.historySinger = query.value(1).toUInt();
        song.filePath = query.value(2).toString();
        song.artist = query.value(3).toString();
        song.title = query.value(4).toString();
        song.songid = query.value(5).toString();
        song.keyChange = query.value(6).toInt();
        song.plays = query.value(7).toUInt();
        song.lastPlayed = query.value(8).toDateTime();
        songs.emplace_back(song);
    }
    return songs;
}

void TableModelHistorySongs::refresh() {
    loadSinger(m_currentSinger);
}


QVariant TableModelHistorySongs::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role == Qt::FontRole) {
        return m_headerFont;
    }
    if (role == Qt::SizeHintRole && orientation == Qt::Horizontal)
        return getSizeHint(section);
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal) {
        return getColumnName(section);
    }
    return {};
}

QString TableModelHistorySongs::getColumnName(int section) const {
    switch (section) {
        case SONG_ID:
            return "id";
        case SINGER_ID:
            return "historySingerId";
        case PATH:
            return "filepath";
        case ARTIST:
            return "Artist";
        case TITLE:
            return "Title";
        case SONGID:
            return "SongID";
        case KEY_CHANGE:
            return "Key";
        case SUNG_COUNT:
            return "Plays";
        case LAST_SUNG:
            return "Last Play";
        default:
            return {};
    }
}

QVariant TableModelHistorySongs::getSizeHint(int section) const {
    switch (section) {
        case KEY_CHANGE:
            return QSize(m_itemFontMetrics.horizontalAdvance(" Key "), m_itemHeight);
        case SUNG_COUNT:
            return QSize(m_itemFontMetrics.horizontalAdvance("_Plays_"), m_itemHeight);
        case LAST_SUNG:
            return QSize(m_itemFontMetrics.horizontalAdvance("_10:00_10/00/00_PM"), m_itemHeight);
        case SONGID:
            return QSize(m_itemFontMetrics.horizontalAdvance("XXXX0000000-01-00"), m_itemHeight);
        case ARTIST:
            return QSize(m_itemFontMetrics.horizontalAdvance("_Artist_"), m_itemHeight);
        case TITLE:
            return QSize(m_itemFontMetrics.horizontalAdvance("_Title_"), m_itemHeight);
        default:
            return {};
    }
}


void TableModelHistorySongs::sort(int column, Qt::SortOrder order) {
    m_lastSortColumn = column;
    m_lastSortOrder = order;
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    const auto oldIndexes = persistentIndexList();
    QList<unsigned int> songIds;
    songIds.reserve(oldIndexes.size());
    for (const auto &index : oldIndexes)
        songIds.append(m_songs[m_visibleRows[index.row()]].id);

    sortSongs();
    rebuildVisibleRows();

    if (!oldIndexes.isEmpty()) {
        QHash<unsigned int, int> rowsById;
        for (int row = 0; row < rowCount({}); ++row)
            rowsById.insert(m_songs[m_visibleRows[row]].id, row);
        QModelIndexList newIndexes;
        newIndexes.reserve(oldIndexes.size());
        for (qsizetype i = 0; i < oldIndexes.size(); ++i)
            newIndexes.append(index(rowsById.value(songIds[i], -1), oldIndexes[i].column()));
        changePersistentIndexList(oldIndexes, newIndexes);
    }
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void TableModelHistorySongs::sortSongs() {
    std::sort(m_songs.begin(), m_songs.end(), [this](const auto &left, const auto &right) {
        const bool ascending = m_lastSortOrder == Qt::AscendingOrder;
        const auto &a = ascending ? left : right;
        const auto &b = ascending ? right : left;
        switch (m_lastSortColumn) {
            case ARTIST: return QString::compare(a.artist, b.artist, Qt::CaseInsensitive) < 0;
            case TITLE: return QString::compare(a.title, b.title, Qt::CaseInsensitive) < 0;
            case SONGID: return QString::compare(a.songid, b.songid, Qt::CaseInsensitive) < 0;
            case KEY_CHANGE: return a.keyChange < b.keyChange;
            case SUNG_COUNT: return a.plays < b.plays;
            case LAST_SUNG: return a.lastPlayed < b.lastPlayed;
            default: return a.id < b.id;
        }
    });
}

void TableModelHistorySongs::setFont(const QFont &font) {
    m_itemFont = font;
    m_headerFont = font;
    m_headerFont.setBold(true);
    m_itemFontMetrics = QFontMetrics(m_itemFont);
    m_itemHeight = m_itemFontMetrics.height() + 6;
}
