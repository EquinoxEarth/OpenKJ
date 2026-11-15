#ifndef OKJSONGBOOKAPI_H
#define OKJSONGBOOKAPI_H

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QSet>
#include <functional>
#include "settings.h"
#include "songbookupload.h"
#include <spdlog/spdlog.h>



class OkjsRequest
{
public:
    int requestId;
    QString singer;
    QString artist;
    QString title;
    int key;
    int time;
    bool operator == (const OkjsRequest& r) const;
};


typedef QList<OkjsRequest> OkjsRequests;

class OkjsVenue
{
public:
    int venueId;
    QString name;
    QString urlName;
    bool accepting;
    bool operator == (const OkjsVenue& v) const;
};



typedef QList<OkjsVenue> OkjsVenues;

class OKJSongbookAPI : public QObject
{
    Q_OBJECT
private:
    std::string m_loggingPrefix{"[SongbookAPI]"};
    std::shared_ptr<spdlog::logger> m_logger;
    int serial;
    OkjsVenues venues;
    bool venuesLoaded = false;
    OkjsRequests requests;
    QNetworkAccessManager *manager;
    QTimer *timer;
    QTimer *alertTimer;
    QElapsedTimer syncAge;
    bool delayErrorEmitted;
    bool connectionReset;
    int entitledSystems;
    bool programIsIdle;
    Settings m_settings;
    okj::SongbookUpload m_songDbUpload{this};
    struct Configuration {
        QUrl url;
        QString apiKey;
        int venue;
        int system;
        bool enabled;
        bool ignoreCertificates;
        bool operator==(const Configuration &) const = default;
    };
    Configuration configuration;
    quint64 generation = 0;
    int deadlineMs;
    bool sslErrorReported = false;
    QSet<QString> polls;
    QSet<QString> repeatPolls;
    QSet<QNetworkReply *> activeReplies;

    Configuration currentConfiguration();
    void post(const QString &command, QJsonObject payload, bool poll, bool repeat = false);
    void handleReply(const QString &command, const QJsonObject &response);
    void refreshLists();
    void markSynchronized();

public:
    explicit OKJSongbookAPI(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr,
                            int requestDeadlineMs = 10000);
    void configurationChanged();
    void getSerial();
    void refreshRequests();
    void removeRequest(int requestId);
    bool getAccepting();
    void setAccepting(bool enabled);
    void refreshVenues();
    void clearRequests();
    void updateSongDb();
    static void testConnection(QNetworkAccessManager &network, const QUrl &url, const QString &apiKey,
                               bool ignoreCertificates, std::function<void(QString)> finished,
                               int deadlineMs = 10000);
    void alertCheck();
    void getEntitledSystemCount();
    [[nodiscard]] int entitledSystemCount() const { return entitledSystems; }
    void triggerTestAdd();

signals:
    void venuesChanged(OkjsVenues);
    void sslError();
    void remoteSongDbUpdatePrepared(int totalBatches);
    void remoteSongDbUpdateProgress(int confirmedBatches);
    void remoteSongDbUpdateFinished(const okj::SongbookUploadResult &result);
    void requestsChanged(OkjsRequests);
    void synchronized(QTime);
    void delayError(int);
    void requestActionFailed(QString message);
    void alertRecieved(QString title, QString message);
    void entitledSystemCountChanged(int count);


public slots:
    void dbUpdateCanceled();
    void setInterval(int interval);


private slots:
        void timerTimeout();
        void alertTimerTimeout();
        void idleStateChanged(bool isIdle);
};

#endif // OKJSONGBOOKAPI_H
