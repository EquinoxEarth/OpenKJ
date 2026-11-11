#ifndef OPENKJ_SONGBOOKUPLOAD_H
#define OPENKJ_SONGBOOKUPLOAD_H

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace okj {

struct SongbookUploadOptions {
    QString databasePath;
    QUrl url;
    QString apiKey;
    int systemId{1};
    bool ignoreCertErrors{false};
};

struct SongbookUploadResult {
    enum class Status { Completed, Cancelled, Failed };
    enum class Catalog { Untouched, ClearUnconfirmed, Cleared };

    Status status;
    Catalog catalog;
    int confirmedBatches{0};
    int totalBatches{0};
    QString error;
};

class SongbookUpload : public QObject
{
    Q_OBJECT
public:
    explicit SongbookUpload(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr,
                            int deadlineMs = 30000);
    ~SongbookUpload() override;
    void start(SongbookUploadOptions options);
    void cancel();

signals:
    void prepared(int totalBatches);
    void progress(int confirmedBatches);
    void finished(const okj::SongbookUploadResult &result);

private:
    struct Upload;
    QNetworkAccessManager *m_network;
    int m_deadlineMs;
    std::shared_ptr<Upload> m_upload;
    QPointer<QNetworkReply> m_reply;

    void sendNext();
    void finish(SongbookUploadResult::Status status, const QString &error = {});
};

} // namespace okj

#endif
