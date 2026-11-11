#include "tablemodelqueuesongs.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QTime>
#include <QMimeData>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QSvgRenderer>
#include "logging.h"

std::ostream & operator<<(std::ostream& os, const QString& s);

TableModelQueueSongs::TableModelQueueSongs(TableModelKaraokeSongs &karaokeSongsModel, QObject *parent)
        : QAbstractTableModel(parent), m_karaokeSongsModel(karaokeSongsModel) {
    m_logger = spdlog::get("logger");
    setFont(m_settings.applicationFont());
}

QVariant TableModelQueueSongs::headerData(int section, Qt::Orientation orientation, int role) const {
    switch (role) {
        case Qt::SizeHintRole:
            if (orientation == Qt::Horizontal)
                return getColumnSizeHint(section);
            return {};
        case Qt::FontRole:
            return m_headerFont;
        case Qt::DisplayRole:
            if (orientation == Qt::Horizontal)
                return getColumnName(section);
            return {};
        default:
            return {};
    }
}

QString TableModelQueueSongs::getColumnName(int section) {
    switch (section) {
        case COL_ID:
            return "ID";
        case COL_DBSONGID:
            return "DBSongId";
        case COL_ARTIST:
            return "Artist";
        case COL_TITLE:
            return "Title";
        case COL_SONGID:
            return "SongID";
        case COL_KEY:
            return "Key";
        case COL_DURATION:
            return "Time";
        default:
            return {};
    }
}

int TableModelQueueSongs::rowCount([[maybe_unused]]const QModelIndex &parent) const {
    return static_cast<int>(m_songs.size());
}

int TableModelQueueSongs::columnCount([[maybe_unused]]const QModelIndex &parent) const {
    return 8;
}

QVariant TableModelQueueSongs::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount())
        return {};
    switch (role) {
        case Qt::FontRole:
            if (m_songs.at(index.row()).played)
                return m_itemFontStrikeout;
            return m_itemFont;
        case Qt::ForegroundRole:
            if (m_songs.at(index.row()).played)
                return QColor("darkGrey");
            return {};
        case Qt::TextAlignmentRole:
            return getColumnTextAlignmentRoleData(index.column());
        case Qt::UserRole: {
            QVariant retVal;
            retVal.setValue(m_songs.at(index.row()));
            return retVal;
        }
        case Qt::DisplayRole:
            return getItemDisplayRoleData(index);
        default:
            return {};
    }
}

QVariant TableModelQueueSongs::getColumnTextAlignmentRoleData(int column) {
    switch (column) {
        case COL_KEY:
            return static_cast<int>(Qt::AlignHCenter | Qt::AlignVCenter);
        case COL_DURATION:
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return {};
    }
}

QVariant TableModelQueueSongs::getItemDisplayRoleData(const QModelIndex &index) const {
    switch (index.column()) {
        case COL_ID:
            return m_songs.at(index.row()).id;
        case COL_DBSONGID:
            return m_songs.at(index.row()).dbSongId;
        case COL_ARTIST:
            return m_songs.at(index.row()).artist;
        case COL_TITLE:
            return m_songs.at(index.row()).title;
        case COL_SONGID:
            if (m_songs.at(index.row()).songId == "!!DROPPED!!")
                return {};
            return m_songs.at(index.row()).songId;
        case COL_KEY:
            if (m_songs.at(index.row()).keyChange == 0)
                return {};
            else if (m_songs.at(index.row()).keyChange > 0)
                return "+" + QString::number(m_songs.at(index.row()).keyChange);
            else
                return m_songs.at(index.row()).keyChange;
        case COL_DURATION:
            if (m_songs.at(index.row()).duration < 1)
                return {};
            return QTime(0, 0, 0, 0).addMSecs(m_songs.at(index.row()).duration).toString("m:ss");
        case COL_PATH:
            return m_songs.at(index.row()).path;
        default:
            return {};
    }
}

void TableModelQueueSongs::loadSinger(const int singerId) {
    m_logger->debug("{} loadSinger({}) requesting data from background DatabaseWorker", m_loggingPrefix, singerId);
    beginResetModel();
    m_curSingerId = singerId;
    ++m_queueRequestId;
    m_queueLoaded = false;
    m_songs.clear();
    m_expectedState.clear();
    endResetModel();
    if (singerId >= 0)
        emit requestLoadQueue(singerId, m_queueRequestId);
}

void TableModelQueueSongs::onQueueLoaded(int singerId, quint64 requestId, const QList<okj::QueueSong> &songs) {
    if (singerId != m_curSingerId || requestId != m_queueRequestId || m_queueLoaded || singerId < 0)
        return;
    if (std::any_of(songs.begin(), songs.end(), [singerId](const auto &song) { return song.singerId != singerId; }))
        return;
    beginResetModel();
    m_expectedState = songs;
    m_songs.assign(songs.begin(), songs.end());
    m_queueLoaded = true;
    endResetModel();
    emit queueModified(m_curSingerId);
}

int TableModelQueueSongs::getPosition(const int songId) {
    auto it = std::find_if(m_songs.begin(), m_songs.end(), [&songId](okj::QueueSong &song) {
        return (song.id == songId);
    });
    if (it == m_songs.end())
        return -1;
    return it->position;
}

bool TableModelQueueSongs::getPlayed(const int songId) {
    auto it = std::find_if(m_songs.begin(), m_songs.end(), [&songId](okj::QueueSong &song) {
        return (song.id == songId);
    });
    if (it == m_songs.end())
        return false;
    return it->played;
}

int TableModelQueueSongs::getKey(const int songId) {
    auto it = std::find_if(m_songs.begin(), m_songs.end(), [&songId](okj::QueueSong &song) {
        return (song.id == songId);
    });
    if (it == m_songs.end())
        return 0;
    return it->keyChange;
}

void TableModelQueueSongs::move(const int oldPosition, const int newPosition) {
    if (!isQueueReady() || oldPosition < 0 || newPosition < 0 ||
        oldPosition >= rowCount() || newPosition >= rowCount() || oldPosition == newPosition)
        return;
    emit layoutAboutToBeChanged();
    if (oldPosition > newPosition) {
        // moving up
        std::for_each(m_songs.begin(), m_songs.end(), [&oldPosition, &newPosition](okj::QueueSong &song) {
            if (song.position == oldPosition)
                song.position = newPosition;
            else if (song.position >= newPosition && song.position < oldPosition)
                song.position++;
        });
    } else {
        // moving down
        std::for_each(m_songs.begin(), m_songs.end(), [&oldPosition, &newPosition](okj::QueueSong &song) {
            if (song.position == oldPosition)
                song.position = newPosition;
            else if (song.position > oldPosition && song.position <= newPosition)
                song.position--;
        });
    }
    std::sort(m_songs.begin(), m_songs.end(), [](okj::QueueSong &a, okj::QueueSong &b) {
        return (a.position < b.position);
    });
    emit layoutChanged();
    commitChanges();
    emit queueModified(m_curSingerId);
}

void TableModelQueueSongs::moveSongId(const int songId, const int newPosition) {
    move(getPosition(songId), newPosition);
}

int TableModelQueueSongs::add(const int songId) {
    if (!isQueueReady())
        return -1;
    okj::KaraokeSong ksong = m_karaokeSongsModel.getSong(songId);
    QSqlQuery query;
    query.prepare("INSERT INTO queuesongs (singer,song,artist,title,discid,path,keychg,played,position) "
                  "VALUES (:singerId,:songId,:songId,:songId,:songId,:songId,:key,:played,:position)");
    query.bindValue(":singerId", m_curSingerId);
    query.bindValue(":songId", songId);
    query.bindValue(":key", 0);
    query.bindValue(":played", false);
    query.bindValue(":position", (int) m_songs.size());
    if (!query.exec()) {
        m_logger->error("{} Queue insertion failed: {}", m_loggingPrefix, query.lastError().text().toStdString());
        emit databaseWriteError(query.lastError().text());
        return -1;
    }
    auto queueSongId = query.lastInsertId().toInt();
    emit layoutAboutToBeChanged();
    m_songs.emplace_back(okj::QueueSong{
            queueSongId,
            m_curSingerId,
            songId,
            false,
            0,
            (int) m_songs.size(),
            ksong.artist,
            ksong.title,
            ksong.songid,
            ksong.duration,
            ksong.path
    });
    m_expectedState.append(m_songs.back());
    emit layoutChanged();
    emit queueModified(m_curSingerId);
    return queueSongId;
}

void TableModelQueueSongs::insert(const int songId, const int position) {
    if (!isQueueReady())
        return;
    if (add(songId) < 0)
        return;
    move(static_cast<int>(m_songs.size()) - 1, position);
}

void TableModelQueueSongs::remove(const int songId) {
    if (!isQueueReady() || getPosition(songId) < 0)
        return;
    emit layoutAboutToBeChanged();
    auto it = std::remove_if(m_songs.begin(), m_songs.end(), [&songId](okj::QueueSong &song) {
        return (song.id == songId);
    });
    m_songs.erase(it, m_songs.end());
    int pos{0};
    std::for_each(m_songs.begin(), m_songs.end(), [&pos](okj::QueueSong &song) {
        song.position = pos++;
    });
    emit layoutChanged();
    commitChanges();
    emit queueModified(m_curSingerId);
}

void TableModelQueueSongs::setKey(const int songId, const int semitones) {
    if (!isQueueReady())
        return;
    auto it = std::find_if(m_songs.begin(), m_songs.end(), [&songId](okj::QueueSong &song) {
        return (song.id == songId);
    });
    if (it == m_songs.end())
        return;
    for (auto &song : m_expectedState)
        if (song.id == songId) song.keyChange = semitones;
    it->keyChange = semitones;
    emit dataChanged(this->index(it->position, COL_KEY), this->index(it->position, COL_KEY),
                     QVector<int>{Qt::DisplayRole});
    emit requestSetQueueSongKey(m_curSingerId, songId, semitones);
}

void TableModelQueueSongs::setPlayed(const int songId, const bool played) {
    if (!isQueueReady() || getPosition(songId) < 0)
        return;
    setPlayedForSinger(m_curSingerId, songId, played);
}

void TableModelQueueSongs::setPlayedForSinger(int singerId, int songId, bool played) {
    if (singerId < 0)
        return;
    m_logger->debug("{} Setting songId {} to played", m_loggingPrefix, songId);
    auto it = std::find_if(m_songs.begin(), m_songs.end(), [&songId](okj::QueueSong &song) {
        return (song.id == songId);
    });
    if (singerId == m_curSingerId && isQueueReady() && it != m_songs.end()) {
        for (auto &song : m_expectedState)
            if (song.id == songId) song.played = played;
        it->played = played;
        emit dataChanged(this->index(it->position, 0), this->index(it->position, columnCount() - 1),
                         QVector<int>{Qt::FontRole, Qt::BackgroundRole, Qt::ForegroundRole});
    }
    emit requestSetQueueSongPlayed(singerId, songId, played);
    // A pending SELECT may predate this write. Request a new snapshot after it.
    if (singerId == m_curSingerId && !isQueueReady())
        loadSinger(singerId);
}

void TableModelQueueSongs::removeAll() {
    if (!isQueueReady())
        return;
    emit layoutAboutToBeChanged();
    m_songs.clear();
    m_songs.shrink_to_fit();
    emit layoutChanged();
    emit queueModified(m_curSingerId);
    m_expectedState.clear();
    emit requestRemoveAllQueueSongs(m_curSingerId);
}

void TableModelQueueSongs::commitChanges() {
    if (!isQueueReady())
        return;
    QList<okj::QueueSong> list;
    for (const auto &song : m_songs) {
        list.append(song);
    }
    const auto expected = m_expectedState;
    m_expectedState = list;
    emit requestCommitQueue(m_curSingerId, list, expected);
}

void TableModelQueueSongs::songAddSlot(int songId, int singerId, int keyChg) {
    if (singerId < 0)
        return;
    if (singerId == m_curSingerId && isQueueReady()) {
        int queueSongId = add(songId);
        setKey(queueSongId, keyChg);
    } else {
        emit requestAddSongToQueue(singerId, songId, keyChg);
        if (singerId == m_curSingerId)
            loadSinger(singerId);
    }
}


QStringList TableModelQueueSongs::mimeTypes() const {
    QStringList types;
    types << "integer/songid";
    types << "text/queueitems";
    return types;
}

QMimeData *TableModelQueueSongs::mimeData(const QModelIndexList &indexes) const {
    auto mimeData = new QMimeData();
    if (!isQueueReady())
        return mimeData;
    if (indexes.size() > 1) {
        QJsonArray jArr;
        std::for_each(indexes.begin(), indexes.end(), [&](QModelIndex index) {
            // Just using COL_ARTIST here because it's the first column that's included in the index list
            if (index.column() != COL_ARTIST)
                return;
            jArr.append(index.sibling(index.row(), 0).data().toInt());
        });
        QJsonDocument jDoc(jArr);
        mimeData->setData("text/queueitems", jDoc.toJson());
    }
    return mimeData;
}

bool TableModelQueueSongs::canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                           const QModelIndex &parent) const {
    if (!isQueueReady())
        return false;
    if ((data->hasFormat("integer/songid")) || (data->hasFormat("text/queueitems")) ||
        data->hasFormat("text/uri-list")) {
        return true;
    }
    return false;
}

bool TableModelQueueSongs::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                                        const QModelIndex &parent) {
    if (getSingerId() == -1) {
        emit songDroppedWithoutSinger();
        return false;
    }
    if (!isQueueReady())
        return false;
    if (action == Qt::MoveAction && data->hasFormat("text/queueitems")) {
        QJsonDocument jDoc = QJsonDocument::fromJson(data->data("text/queueitems"));
        QJsonArray jArr = jDoc.array();
        auto ids = jArr.toVariantList();
        if (ids.empty() || std::any_of(ids.begin(), ids.end(), [this](const auto &id) {
                return getPosition(id.toInt()) < 0;
            }))
            return false;
        int dropRow{0};
        if (parent.row() >= 0)
            dropRow = parent.row();
        else if (row >= 0)
            dropRow = row;
        else
            dropRow = rowCount() - 1;
        if (getPosition(ids.at(0).toInt()) > dropRow)
            std::reverse(ids.begin(), ids.end());
        std::for_each(ids.begin(), ids.end(), [&](auto val) {
            int oldPosition = getPosition(val.toInt());
            if (oldPosition < dropRow && dropRow != rowCount() - 1)
                moveSongId(val.toInt(), dropRow - 1);
            else
                moveSongId(val.toInt(), dropRow);
        });
        if (dropRow == rowCount() - 1) {
            // moving to bottom
            emit qSongsMoved(dropRow - ids.size() + 1, 0, rowCount() - 1, columnCount() - 1);
        } else if (getPosition(ids.at(0).toInt()) < dropRow) {
            // moving down
            emit qSongsMoved(dropRow - ids.size(), 0, dropRow - 1, columnCount() - 1);
        } else {
            // moving up
            emit qSongsMoved(dropRow, 0, dropRow + ids.size() - 1, columnCount() - 1);
        }
        commitChanges();
        return true;
    }
    if (data->hasFormat("integer/songid")) {
        unsigned int dropRow;
        if (parent.row() >= 0)
            dropRow = parent.row();
        else if (row >= 0)
            dropRow = row;
        else
            dropRow = rowCount();
        int songid;
        QByteArray bytedata = data->data("integer/songid");
        songid = QString(bytedata.data()).toInt();
        insert(songid, static_cast<int>(dropRow));
        commitChanges();
        return true;
    } else if (data->hasFormat("text/uri-list")) {
        QList<QUrl> items = data->urls();
        if (!items.empty()) {
            unsigned int droprow;
            if (parent.row() >= 0) {
                droprow = parent.row();
            } else if (row >= 0) {
                droprow = row;
            } else {
                droprow = rowCount();
            }
            emit filesDroppedOnSinger(items, m_curSingerId, static_cast<int>(droprow));
        }
        commitChanges();
        return true;
    }
    return false;
}

Qt::DropActions TableModelQueueSongs::supportedDropActions() const {
    return Qt::CopyAction | Qt::MoveAction;
}

Qt::ItemFlags TableModelQueueSongs::flags([[maybe_unused]]const QModelIndex &index) const {
    if (!isQueueReady())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;

}

void TableModelQueueSongs::sort(int column, Qt::SortOrder order) {
    if (!isQueueReady())
        return;
    emit layoutAboutToBeChanged();
    if (order == Qt::AscendingOrder) {
        std::sort(m_songs.begin(), m_songs.end(), [&column](okj::QueueSong &a, okj::QueueSong &b) {
            switch (column) {
                case COL_ARTIST:
                    return (a.artist < b.artist);
                case COL_TITLE:
                    return (a.title < b.title);
                case COL_SONGID:
                    return (a.songId < b.songId);
                case COL_DURATION:
                    return (a.duration < b.duration);
                case COL_KEY:
                    return (a.keyChange < b.keyChange);
                default:
                    return (a.position < b.position);
            }
        });
    } else {
        std::sort(m_songs.rbegin(), m_songs.rend(), [&column](okj::QueueSong &a, okj::QueueSong &b) {
            switch (column) {
                case COL_ARTIST:
                    return (a.artist < b.artist);
                case COL_TITLE:
                    return (a.title < b.title);
                case COL_SONGID:
                    return (a.songId < b.songId);
                case COL_DURATION:
                    return (a.duration < b.duration);
                case COL_KEY:
                    return (a.keyChange < b.keyChange);
                default:
                    return (a.position < b.position);
            }
        });
    }
    int pos{0};
    std::for_each(m_songs.begin(), m_songs.end(), [&pos](okj::QueueSong &song) {
        song.position = pos++;
    });
    emit layoutChanged();
    commitChanges();
}

void TableModelQueueSongs::setFont(const QFont &font) {
    m_itemFont = font;
    m_itemFontStrikeout = font;
    m_itemFontStrikeout.setStrikeOut(true);
    m_itemHeight = m_itemFontMetrics.height() + 6;
    m_headerFont = font;
    m_headerFont.setBold(true);
}

QSize TableModelQueueSongs::getColumnSizeHint(int section) const {
    switch (section) {
        case COL_ID:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_ID").width(), m_itemHeight);
        case COL_ARTIST:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_Artist").width(), m_itemHeight);
        case COL_TITLE:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_Title").width(), m_itemHeight);
        case COL_SONGID:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "XXXX0000000-01-00").width(), m_itemHeight);
        case COL_KEY:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_Key_").width(), m_itemHeight);
        case COL_DURATION:
            return QSize(m_itemFontMetrics.size(Qt::TextSingleLine, "_00:00").width(), m_itemHeight);
        case COL_PATH:
            // This is actually where the delete icon is displayed, reused the column since we don't actually show path info
        default:
            return QSize(m_itemHeight + 6, m_itemHeight);
    }
}

void ItemDelegateQueueSongs::resizeIconsForFont(const QFont &font) {
    QString thm = (m_settings.theme() == 1) ? ":/theme/Icons/okjbreeze-dark/" : ":/theme/Icons/okjbreeze/";
    m_curFontHeight = QFontMetrics(font).height();
    m_iconDelete = QImage(m_curFontHeight, m_curFontHeight, QImage::Format_ARGB32);
    m_iconDelete.fill(Qt::transparent);
    QPainter painterDelete(&m_iconDelete);
    QSvgRenderer svgrndrDelete(thm + "actions/16/edit-delete.svg");
    svgrndrDelete.render(&painterDelete);
}

ItemDelegateQueueSongs::ItemDelegateQueueSongs(QObject *parent) :
        QItemDelegate(parent) {
    resizeIconsForFont(m_settings.applicationFont());
}

void ItemDelegateQueueSongs::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    if (index.column() == TableModelQueueSongs::COL_PATH) {
        int topPad = (option.rect.height() - m_curFontHeight) / 2;
        int leftPad = (option.rect.width() - m_curFontHeight) / 2;
        painter->drawImage(QRect(option.rect.x() + leftPad, option.rect.y() + topPad, m_curFontHeight, m_curFontHeight),
                           m_iconDelete);
        return;
    }
    QItemDelegate::paint(painter, option, index);
}
