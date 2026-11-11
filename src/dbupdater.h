#ifndef DBUPDATER_H
#define DBUPDATER_H

#include <QObject>
#include <QStringList>
#include <QtSql>
#include "src/models/tablemodelkaraokesourcedirs.h"
#include <array>
#include <atomic>
#include <QElapsedTimer>
#include <QFuture>
#include <QMutex>
#include <memory>


class DbUpdater : public QObject
{
    Q_OBJECT

public:
    struct MissingFile {
        int id;
        QString path;
    };

    struct Progress {
        QString status;
        int value{0};
        int maximum{0};
    };

    struct ScanState {
        std::atomic_bool cancelled{false};
        QMutex mutex;
        Progress progress;
    };

    struct ImportSettings {
        bool lazyDurations;
        bool skipValidation;
    };

    struct ScanResult {
        bool success{false};
        bool cancelled{false};
        QStringList errors;
        QList<MissingFile> missing;
    };

private:
    struct DbSongRecord {
       int id{-1};
       bool isDropped{false};
       QString path;
    };

    // file extension list must be sorted and in lower case:
    const std::array<std::string, 9> karaoke_file_extensions {
        "avi",
        "cdg",
        "m4v",
        "mkv",
        "mp4",
        "mpeg",
        "mpg",
        "wmv",
        "zip"
    };

    std::array<std::string, 5> audio_file_extensions {
        "flac",
        "mov",
        "mp3",
        "ogg",
        "wav"
    };

    class DiskEnumerator
    {
    private:
        DbUpdater& m_parent;
        QStringList m_karaokeFilesOnDisk;
        QStringList m_audioFilesOnDisk;
        int m_i_kar{-1};
        int m_i_aud{0};

    public:
        bool IsValid{false};
        QString CurrentFile;
        explicit DiskEnumerator(DbUpdater& parent) : m_parent(parent) { reset(); }
        bool findKaraokeFilesOnDisk();
        void readNextDiskFile();
        void reset() { m_i_kar = -1; m_i_aud = 0; IsValid = false; }
    };

    class DbEnumerator
    {
    private:
        DbUpdater& m_parent;
        QSqlQuery m_dbSongs;

    public:
        bool IsValid = false;
        DbSongRecord CurrentRecord;
        explicit DbEnumerator(DbUpdater& parent) : m_parent(parent), m_dbSongs(parent.m_db) {}
        bool prepareQuery(bool limitToPaths);
        void readNextRecord();
        void finish() { m_dbSongs.finish(); }
    };

    struct ImportRecord {
        QString path, filename, artist, title, discId, search;
        int duration;
    };

    QSqlDatabase m_db;
    const std::atomic_bool *m_cancelled;
    ImportSettings m_importSettings;
    QStringList m_paths;
    QStringList m_errors;
    QList<MissingFile> m_missingFiles;
    QElapsedTimer m_guiUpdateTimer;

    void setPaths(const QList<QString> &paths);
    bool sourcesAvailable();
    bool prepareImports(const QStringList &files, QList<ImportRecord> &imports);
    bool writeImports(const QList<ImportRecord> &imports, const QSet<QString> &moved);
    bool fixMissingFiles(QVector<DbSongRecord> &missing, const QStringList &newFiles, QSet<QString> &moved);
    bool reportSqlError(const QSqlError &error);
    bool cancelled() const { return m_cancelled && m_cancelled->load(); }
    bool progressDue();

public:

    enum ProcessingOption
    {
        None                          = 0x0,
        FixMovedFiles                 = 0x01,
        FixMovedFilesSearchInWholeDB  = FixMovedFiles | 0x02,
        PrepareForRemovalOfMissing    = 0x04,
        SkipUnavailableSources        = 0x08
    };
    Q_DECLARE_FLAGS(ProcessingOptions, ProcessingOption)

    static QFuture<ScanResult> scanAsync(const QString &databasePath, const QStringList &paths,
                                         ProcessingOptions options, std::shared_ptr<ScanState> state);
    static QFuture<ScanResult> removeMissingAsync(const QString &databasePath, const QStringList &paths,
                                                  const QList<MissingFile> &files, std::shared_ptr<ScanState> state);
    explicit DbUpdater(QObject *parent = nullptr);
    DbUpdater(const QSqlDatabase &db, const std::atomic_bool *cancelled, ImportSettings settings);

    QStringList getErrors();
    bool process(const QList<QString> &paths, ProcessingOptions options);
    bool addFilesToDatabase(const QList<QString> &files);
    int missingFilesCount();
    bool removeMissingFilesFromDatabase();
    bool removeMissingFilesFromDatabase(const QList<MissingFile> &files, const QStringList &paths);
    const QList<MissingFile> &missingFiles() const { return m_missingFiles; }

signals:
    void stateChanged(QString state);
    void progressChanged(int progress, int max);

};

Q_DECLARE_OPERATORS_FOR_FLAGS(DbUpdater::ProcessingOptions)

#endif // DBUPDATER_H
