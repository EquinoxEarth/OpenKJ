#ifndef DATABASEWORKER_H
#define DATABASEWORKER_H

#include <QObject>
#include <functional>
#include <QSqlDatabase>
#include <QList>
#include <QString>
#include "okjtypes.h"
#include "settings.h"

class DatabaseWorker : public QObject
{
    Q_OBJECT

public:
    explicit DatabaseWorker(const QString &dbPath, QObject *parent = nullptr);
    ~DatabaseWorker() override;

public slots:
    void init(); // Initialize connection on worker thread
    void loadRotation();
    void loadQueue(int singerId, quint64 requestId);
    
    // Write Slots
    void addSinger(const QString &name, int positionHint, bool makeRegular);
    void addSingerWithSong(const QString &name, int positionHint, int songId, int keyChange, bool makeRegular = false);
    void moveSinger(int oldPosition, int newPosition);
    void renameSinger(int singerId, const QString &newName);
    void deleteSinger(int singerId);
    void setSingerRegular(int singerId, bool isRegular);
    void clearRotation();
    
    void addSongToQueue(int singerId, int songId, int keyChange);
    void moveQueueSong(int singerId, int oldPosition, int newPosition);
    void deleteQueueSong(int singerId, int queueSongId);
    void setQueueSongKey(int singerId, int songId, int key);
    void setQueueSongPlayed(int singerId, int songId, bool played);
    void removeAllQueueSongs(int singerId);
    void commitRotation(const QList<okj::RotationSinger> &singers, const QList<okj::RotationSinger> &expected);
    void commitQueue(int singerId, const QList<okj::QueueSong> &songs, const QList<okj::QueueSong> &expected);

signals:
    void rotationLoaded(const QList<okj::RotationSinger> &singers);
    void queueLoaded(int singerId, quint64 requestId, const QList<okj::QueueSong> &songs);
    void singerAdded(int singerId, const QString &name, int positionHint);
    void rotationUpdated();
    void queueUpdated(int singerId);
    void writeFailed(int singerId); // -1 means rotation; reload optimistic GUI state.
    void errorOccurred(const QString &errorMsg);

private:
    bool write(int singerId, const std::function<void()> &operation);
    QString m_dbPath;
    QSqlDatabase m_db;
    Settings m_settings;
};

#endif // DATABASEWORKER_H
