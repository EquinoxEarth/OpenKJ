#ifndef TABLEMODELBREAKSONGSNEW_H
#define TABLEMODELBREAKSONGSNEW_H

#include <QAbstractTableModel>
#include <QHash>
#include <QTime>
#include <chrono>
#include <cstddef>
#include <vector>
#include <spdlog/async_logger.h>
#include "spdlog/spdlog.h"
#include "logging.h"
#include "settings.h"

struct BreakSong {
    int id{0};
    QString artist;
    QString title;
    QString path;
    QString filename;
    int duration;
    std::string searchString;

};


class TableModelBreakSongs : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum {COL_ID=0,COL_ARTIST,COL_TITLE,COL_FILENAME,COL_DURATION};
    explicit TableModelBreakSongs(QObject *parent = nullptr);
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order) override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    // Requires an existing song ID; throws std::out_of_range if absent.
    BreakSong &getSong(const int breakSongId);
    int getSongId(const QString &filePath);
    void loadDatabase();
    void search(const QString &searchStr);


private:
    std::string m_loggingPrefix{"[BreakSongsModel]"};
    std::shared_ptr<spdlog::logger> m_logger;
    std::vector<BreakSong> m_filteredSongs;
    std::vector<BreakSong> m_allSongs;
    // Rows refer to library storage, independent of filtered/display order.
    QHash<int, std::size_t> m_songRowsById;
    QHash<QString, int> m_songIdsByPath;
    QString m_lastSearch;
    Qt::SortOrder m_lastSortOrder{Qt::AscendingOrder};
    int m_lastSortColumn{1};
    Settings m_settings;

};

#endif // TABLEMODELBREAKSONGSNEW_H
