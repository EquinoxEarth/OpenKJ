#ifndef SONGSHOP_H
#define SONGSHOP_H

#include <QNetworkAccessManager>
#include <QObject>
#include "settings.h"
#include <spdlog/logger.h>

class ShopSong
{
public:
    operator QString() const { return QString("Artist: " + artist + " Title: " + title + " SongId: " + songid + " Vendor: " + vendor + " Price: " + QString("%1").arg(price)); }
    QString artist;
    QString title;
    QString songid;
    QString vendor;
    int type;
    double price;
    bool operator == (const ShopSong r) const;
};

using ShopSongs = QList<ShopSong>;

class SongShop : public QObject
{
    Q_OBJECT

public:
    explicit SongShop(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    void updateCache();
    ShopSongs getSongs();
    void knLogin(const QString &userName, const QString &password);
    void knPurchase(const QString &songId, const QString &artist, const QString &title,
                    const QString &ccNumber, const QString &ccM, const QString &ccY, const QString &ccCVV);

signals:
    void songUpdateStarted();
    void songsUpdated();
    void knLoginSuccess();
    void knLoginFailure(const QString &message);
    void karaokeSongDownloaded(const QString &path);
    void paymentProcessingFailed(const QString &message);
    void downloadFailed(const QString &message);
    void downloadProgress(qint64 received, qint64 total);

private:
    std::shared_ptr<spdlog::logger> m_logger;
    QNetworkAccessManager *m_network;
    bool m_catalogLoading{false};
    bool m_catalogLoaded{false};
    bool m_loginPending{false};
    bool m_purchasePending{false};
    ShopSongs m_songs;
    QString m_session;
    Settings m_settings;

    void downloadFile(const QUrl &url, const QString &filename, bool zip);
    void completeDownload(const QString &path, const QString &error);
};

#endif // SONGSHOP_H
