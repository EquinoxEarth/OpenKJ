#include "songshop.h"
#include "localfilename.h"
#include "mzarchive.h"
#include "networkrequests.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QtConcurrentRun>
#include <QtEndian>
#include <spdlog/spdlog.h>
#include <mp4/mp4file.h>
#include <zip.h>

namespace {
struct SongDownload {
    std::unique_ptr<QTemporaryFile> file;
    QString writeError;
};

QUrl partyTymeUrl(const QString &endpoint, std::initializer_list<std::pair<QByteArray, QString>> parameters)
{
    QByteArray query;
    for (const auto &[key, value] : parameters) {
        if (!query.isEmpty())
            query += '&';
        query += key + '=' + QUrl::toPercentEncoding(value);
    }
    QUrl url(QStringLiteral("https://www.partytyme.net/songshop/cat/") + endpoint);
    url.setQuery(QString::fromLatin1(query), QUrl::StrictMode);
    return url;
}

bool isMp4(QFile &file)
{
    // Reject non-media responses and incomplete ISO BMFF boxes, even when
    // the transfer has no Content-Length to compare against.
    const auto fileSize = static_cast<quint64>(file.size());
    bool fileType = false, movie = false, media = false;
    for (quint64 offset = 0; offset < fileSize;) {
        if (!file.seek(static_cast<qint64>(offset)))
            return false;
        const auto header = file.read(8);
        if (header.size() != 8)
            return false;
        const auto type = header.sliced(4);
        quint64 size = qFromBigEndian<quint32>(header.constData());
        quint64 headerSize = 8;
        if (size == 1) {
            const auto extended = file.read(8);
            if (extended.size() != 8)
                return false;
            size = qFromBigEndian<quint64>(extended.constData());
            headerSize = 16;
        } else if (size == 0) {
            size = fileSize - offset; // A final box may extend to EOF.
        }
        if (size < headerSize || size > fileSize - offset)
            return false;
        const auto payloadSize = size - headerSize;
        fileType |= type == "ftyp" && payloadSize >= 8;
        movie |= type == "moov" && payloadSize >= 8;
        media |= type == "mdat" && payloadSize > 0;
        offset += size;
    }
    if (!fileType || !movie || !media)
        return false;
#ifdef Q_OS_WIN
    const auto path = file.fileName().toStdWString();
#else
    const auto path = QFile::encodeName(file.fileName()).toStdString();
#endif
    const TagLib::MP4::File track(path.c_str());
    const auto *audio = track.audioProperties();
    return track.isValid() && audio && (audio->lengthInMilliseconds() > 0 || audio->sampleRate() > 0);
}

bool isKaraokeZip(const QString &path)
{
    auto *archive = zip_open(QFile::encodeName(path).constData(), ZIP_RDONLY | ZIP_CHECKCONS, nullptr);
    if (!archive)
        return false;
    zip_discard(archive);
    return MzArchive(path).isValidKaraokeFile(false);
}

QString saveDownload(const QString &staged, const QString &destination, bool zip)
{
    const auto cleanup = qScopeGuard([&] { QFile::remove(staged); });
    QFile input(staged);
    if (!input.open(QIODevice::ReadOnly))
        return SongShop::tr("Could not read the downloaded file.");
    if (zip ? !isKaraokeZip(staged) : !isMp4(input))
        return SongShop::tr("The server returned an incomplete or invalid karaoke file.");
    if (!input.seek(0))
        return SongShop::tr("Could not read the downloaded file.");

    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
        return SongShop::tr("Could not save the song: %1").arg(output.errorString());

    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (!input.atEnd()) {
        const auto read = input.read(buffer.data(), buffer.size());
        if (read <= 0)
            return SongShop::tr("Could not read the downloaded file.");
        if (output.write(buffer.constData(), read) != read)
            return SongShop::tr("Could not save the song: %1").arg(output.errorString());
    }
    if (!output.commit())
        return SongShop::tr("Could not save the song: %1").arg(output.errorString());
    return {};
}
}

SongShop::SongShop(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent),
      m_logger(spdlog::get("logger")),
      m_network(network ? network : new QNetworkAccessManager(this))
{
    if (!m_logger)
        m_logger = spdlog::default_logger();
}

void SongShop::updateCache()
{
    if (m_catalogLoading)
        return;
    m_catalogLoading = true;
    auto request = okj::httpsRequest(QUrl("https://db.openkj.org/apigetsongs_v2"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const auto body = QJsonDocument(QJsonObject{{"command", "getsongs"}}).toJson();
    okj::readJsonReply(m_network->post(request, body), this, 64 * 1024 * 1024, [this](const QJsonDocument &document) {
        const auto response = document.object();
        if (response.value("command").toString() != "getsongs" || response.value("error").toBool() ||
            !response.value("songs").isArray()) {
            m_catalogLoading = false;
            m_logger->warn("[SongShop] Could not load the song catalog");
            return;
        }
        emit songUpdateStarted();
        m_songs.clear();
        for (const auto &entry : response.value("songs").toArray()) {
            const auto object = entry.toObject();
            m_songs.append({object.value("artist").toString(), object.value("title").toString(),
                            object.value("songid").toString(), object.value("vendor").toString(),
                            0, object.value("price").toDouble()});
        }
        m_catalogLoaded = true;
        m_catalogLoading = false;
        emit songsUpdated();
    });
}

ShopSongs SongShop::getSongs()
{
    if (!m_catalogLoaded)
        updateCache();
    return m_songs;
}

void SongShop::knLogin(const QString &userName, const QString &password)
{
    if (m_loginPending || m_purchasePending)
        return;
    m_session.clear();
    m_loginPending = true;
    const auto hash = QCryptographicHash::hash(password.toLocal8Bit(), QCryptographicHash::Md5).toHex();
    const auto url = partyTymeUrl("api_account_setup.php", {
        {"action", "validate_login"}, {"username", userName}, {"md5", QString::fromLatin1(hash)}, {"merchant", "99"}
    });
    okj::readJsonReply(m_network->get(okj::httpsRequest(url)), this, 128 * 1024, [this](const QJsonDocument &document) {
        m_loginPending = false;
        const auto response = document.object();
        const auto session = response.value("session_id").toString();
        if (response.value("result").toString() == "SUCCESS" && !session.isEmpty()) {
            m_session = session;
            emit knLoginSuccess();
            return;
        }
        const bool badCredentials = response.value("error").toString() == "Wrong username or password";
        emit knLoginFailure(badCredentials ? tr("Check your PartyTyme username and password.")
                                          : tr("Could not log in to PartyTyme. Check your connection and try again."));
    });
}

void SongShop::knPurchase(const QString &songId, const QString &artist, const QString &title,
                         const QString &ccNumber, const QString &ccM, const QString &ccY, const QString &ccCVV)
{
    if (m_purchasePending)
        return;
    if (m_session.isEmpty()) {
        emit paymentProcessingFailed(tr("Log in to PartyTyme before placing an order."));
        return;
    }

    m_purchasePending = true;
    const bool zip = songId.contains("PY");
    const auto url = partyTymeUrl("api_make_order.php", {
        {"media_format", zip ? "mp3g" : "mp4"}, {"tracks", songId},
        {"cc_number", ccNumber}, {"cc_cvv", ccCVV}, {"cc_exp_mm", ccM}, {"cc_exp_yyyy", ccY},
        {"session_id", std::exchange(m_session, {})}, {"merchant", "99"}
    });
    const auto filename = QStringLiteral("%1 - %2 - %3%4")
        .arg(okj::safeFilenamePart(songId, "Song"), okj::safeFilenamePart(artist, "Unknown artist"),
             okj::safeFilenamePart(title, "Unknown title"), zip ? ".zip" : ".mp4");
    okj::readJsonReply(m_network->get(okj::httpsRequest(url)), this, 128 * 1024,
                      [this, filename, zip](const QJsonDocument &document) {
        const auto response = document.object();
        if (response.value("result").toString() == "SUCCESS") {
            const auto links = response.value("download_links").toArray();
            downloadFile(links.isEmpty() ? QUrl{} : QUrl(links.first().toString(), QUrl::StrictMode), filename, zip);
            return;
        }
        m_purchasePending = false;
        const auto error = response.value("error").toString();
        if (error == "Payment failed. Check your credit card details.") {
            emit paymentProcessingFailed(tr("Payment was declined. Verify your card details."));
        } else if (error == "User not logged in") {
            emit paymentProcessingFailed(tr("The PartyTyme login expired before checkout."));
        } else {
            emit paymentProcessingFailed(tr("Could not confirm the purchase. Check your PartyTyme order history before trying again."));
        }
    });
}

void SongShop::downloadFile(const QUrl &url, const QString &filename, bool zip)
{
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        completeDownload({}, tr("PartyTyme did not provide a secure download link."));
        return;
    }
    const auto folder = m_settings.storeDownloadDir();
    if (folder.trimmed().isEmpty() || !QDir().mkpath(folder)) {
        completeDownload({}, tr("Could not create the song download folder."));
        return;
    }
    const QDir directory(folder);
    auto download = std::make_shared<SongDownload>();
    download->file = std::make_unique<QTemporaryFile>(directory.absoluteFilePath(".openkj-download-XXXXXX"));
    if (!download->file->open()) {
        completeDownload({}, tr("Could not write to the song download folder: %1").arg(download->file->errorString()));
        return;
    }

    const auto destination = directory.absoluteFilePath(filename);
    auto request = okj::httpsRequest(url, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("Accept-Encoding", "identity");
    auto *reply = m_network->get(request);
    reply->setReadBufferSize(128 * 1024);
    const auto drain = [reply, download] {
        while (download->writeError.isEmpty() && reply->bytesAvailable() > 0) {
            const auto chunk = reply->read(64 * 1024);
            if (chunk.isEmpty() || download->file->write(chunk) != chunk.size())
                download->writeError = tr("Could not write the downloaded song: %1").arg(download->file->errorString());
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, [reply, download, drain] {
        drain();
        if (!download->writeError.isEmpty())
            reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, &SongShop::downloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, download, drain, destination, zip] {
        reply->deleteLater();
        drain();
        auto staged = std::move(download->file);
        const auto contentLength = reply->rawHeader("Content-Length");
        bool sizeValid = false;
        const auto expectedSize = contentLength.toLongLong(&sizeValid);
        QString error = download->writeError;
        if (error.isEmpty() &&
            (!okj::successfulResponse(reply) || reply->url().scheme() != "https" || staged->size() <= 0 ||
             (!contentLength.isEmpty() && (!sizeValid || expectedSize != staged->size())))) {
            error = tr("The song download failed or was incomplete.");
        }
        if (error.isEmpty() && !staged->flush())
            error = tr("Could not finish writing the downloaded song: %1").arg(staged->errorString());
        if (!error.isEmpty()) {
            staged.reset();
            completeDownload({}, error);
            return;
        }

        const auto path = staged->fileName();
        staged->setAutoRemove(false);
        staged.reset();
        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, destination] {
            const auto error = watcher->result();
            watcher->deleteLater();
            completeDownload(destination, error);
        });
        watcher->setFuture(QtConcurrent::run(saveDownload, path, destination, zip));
    });
    QTimer::singleShot(15 * 60 * 1000, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
}

void SongShop::completeDownload(const QString &path, const QString &error)
{
    m_purchasePending = false;
    if (!error.isEmpty()) {
        m_logger->warn("[SongShop] {}", error.toStdString());
        emit downloadFailed(error);
        return;
    }
    m_logger->info("[SongShop] Downloaded purchased track");
    emit karaokeSongDownloaded(path);
}
