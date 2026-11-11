#include "songbookupload.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <utility>

namespace okj {
namespace {

constexpr qsizetype songsPerBatch = 1000;
constexpr qint64 maxReplyBytes = 256 * 1024;

struct PreparedCatalog {
    QList<QJsonArray> batches;
    QString error;
};

PreparedCatalog prepareCatalog(const QString &path, const std::atomic_bool &cancelled)
{
    if (cancelled.load())
        return {};
    const auto connection = QStringLiteral("songbook-upload-") + QUuid::createUuid().toString();
    const auto removeConnection = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
    auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
    database.setDatabaseName(path);
    database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000");
    if (!database.open())
        return {{}, database.lastError().text()};

    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec("SELECT DISTINCT artist, title FROM dbsongs "
                    "WHERE discid NOT IN ('!!DROPPED!!', '!!BAD!!') ORDER BY artist, title"))
        return {{}, query.lastError().text()};

    PreparedCatalog catalog;
    QJsonArray batch;
    while (!cancelled.load() && query.next()) {
        batch.append(QJsonObject{{"artist", query.value(0).toString()}, {"title", query.value(1).toString()}});
        if (batch.size() == songsPerBatch)
            catalog.batches.append(std::exchange(batch, {}));
    }
    if (cancelled.load())
        return {};
    if (query.lastError().isValid())
        return {{}, query.lastError().text()};
    if (!batch.isEmpty())
        catalog.batches.append(std::move(batch));
    return catalog;
}

} // namespace

struct SongbookUpload::Upload {
    SongbookUploadOptions options;
    std::atomic_bool cancelled{false};
    QList<QJsonArray> batches;
    SongbookUploadResult::Catalog catalog{SongbookUploadResult::Catalog::Untouched};
    int confirmedBatches{0};
};

SongbookUpload::SongbookUpload(QObject *parent, QNetworkAccessManager *network, int deadlineMs)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this)),
      m_deadlineMs(deadlineMs)
{
}

SongbookUpload::~SongbookUpload()
{
    if (m_upload)
        m_upload->cancelled.store(true);
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
    }
}

void SongbookUpload::start(SongbookUploadOptions options)
{
    using Result = SongbookUploadResult;
    if (m_upload)
        return;

    const auto job = std::make_shared<Upload>();
    job->options = std::move(options);
    m_upload = job;
    const auto &url = job->options.url;
    if (url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
        finish(Result::Status::Failed, tr("The Songbook server URL is invalid."));
        return;
    }
    if (job->options.databasePath.isEmpty()) {
        finish(Result::Status::Failed, tr("The local song database is unavailable."));
        return;
    }

    auto *watcher = new QFutureWatcher<PreparedCatalog>(this);
    connect(watcher, &QFutureWatcher<PreparedCatalog>::finished, this, [this, watcher, job] {
        watcher->deleteLater();
        if (m_upload != job)
            return;
        auto catalog = watcher->future().takeResult();
        if (!catalog.error.isEmpty()) {
            finish(Result::Status::Failed, tr("Could not read the local song database. %1").arg(catalog.error));
            return;
        }
        job->batches = std::move(catalog.batches);
        emit prepared(static_cast<int>(job->batches.size()));
        if (m_upload == job)
            sendNext();
    });
    watcher->setFuture(QtConcurrent::run([job] {
        return prepareCatalog(job->options.databasePath, job->cancelled);
    }));
}

void SongbookUpload::sendNext()
{
    using Result = SongbookUploadResult;
    const auto job = m_upload;
    const bool clearing = job->catalog == Result::Catalog::Untouched;

    const auto command = clearing ? QStringLiteral("clearDatabase") : QStringLiteral("addSongs");
    QJsonObject payload{{"command", command}, {"api_key", job->options.apiKey}, {"system_id", job->options.systemId}};
    int expectedSongs = 0;
    if (clearing) {
        job->catalog = Result::Catalog::ClearUnconfirmed;
    } else {
        const auto &batch = job->batches.at(job->confirmedBatches);
        expectedSongs = static_cast<int>(batch.size());
        payload.insert("songs", batch);
    }

    QNetworkRequest request(job->options.url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    auto *reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_reply = reply;
    reply->setReadBufferSize(maxReplyBytes + 1);
    if (job->options.ignoreCertErrors)
        connect(reply, &QNetworkReply::sslErrors, reply, [reply] { reply->ignoreSslErrors(); });

    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_reply == reply && reply->bytesAvailable() > maxReplyBytes)
            finish(Result::Status::Failed, tr("The Songbook reply was too large."));
    });
    QTimer::singleShot(m_deadlineMs, reply, [this, reply, context = QPointer<SongbookUpload>(this)] {
        if (context && m_reply == reply && !reply->isFinished())
            finish(Result::Status::Failed, tr("The Songbook request timed out."));
    });

    const auto completed = [this, reply, job, command, clearing, expectedSongs] {
        reply->deleteLater();
        if (m_reply != reply)
            return;
        m_reply.clear();

        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 0 && (status < 200 || status >= 300)) {
            finish(Result::Status::Failed, tr("The Songbook server returned HTTP %1.").arg(status));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            finish(Result::Status::Failed, reply->errorString());
            return;
        }
        if (status == 0 || reply->bytesAvailable() > maxReplyBytes) {
            finish(Result::Status::Failed, tr("The Songbook server returned an invalid HTTP reply."));
            return;
        }

        const auto response = QJsonDocument::fromJson(reply->readAll()).object();
        const auto errorFlag = response.value("error");
        const auto errorText = response.value("errorString").toString().trimmed();
        const auto commandEcho = response.value("command");
        const auto errors = response.value("errors");
        const auto successFlag = response.value("success");
        // The standalone server encodes the error flag as a string.
        const auto errorString = errorFlag.toString().toLower();
        const bool explicitSuccess = (errorFlag.isBool() && !errorFlag.toBool()) || errorString == "false" ||
                                     successFlag.toBool();
        if (errorFlag.toBool() || errorString == "true" || !errorText.isEmpty() ||
            (!errors.isUndefined() && !errors.isNull() && (!errors.isArray() || !errors.toArray().isEmpty())) ||
            (successFlag.isBool() && !successFlag.toBool())) {
            finish(Result::Status::Failed, errorText.isEmpty() ? tr("The Songbook server reported an error.")
                                                              : errorText.left(500));
            return;
        }
        if ((!commandEcho.isUndefined() && commandEcho.toString() != command) ||
            (commandEcho.isUndefined() && !explicitSuccess) ||
            (!errorFlag.isUndefined() && !errorFlag.isNull() && !errorFlag.isBool() && errorString != "false")) {
            finish(Result::Status::Failed, tr("The Songbook server did not acknowledge the request."));
            return;
        }
        if (!clearing && response.contains("entries processed") &&
            response.value("entries processed").toInt(-1) != expectedSongs) {
            finish(Result::Status::Failed, tr("The Songbook server reported a different number of songs in this batch."));
            return;
        }

        if (clearing)
            job->catalog = Result::Catalog::Cleared;
        else
            ++job->confirmedBatches;
        if (job->confirmedBatches == job->batches.size()) {
            finish(Result::Status::Completed);
            return;
        }
        emit progress(job->confirmedBatches);
        if (m_upload == job)
            sendNext();
    };
    connect(reply, &QNetworkReply::finished, this, completed);
    if (reply->isFinished())
        completed();
}

void SongbookUpload::cancel()
{
    if (m_upload)
        finish(SongbookUploadResult::Status::Cancelled);
}

void SongbookUpload::finish(SongbookUploadResult::Status status, const QString &error)
{
    const auto job = std::exchange(m_upload, {});
    job->cancelled.store(true);
    if (const auto reply = std::exchange(m_reply, {}))
        reply->abort();
    emit finished({status, job->catalog, job->confirmedBatches, static_cast<int>(job->batches.size()), error});
}

} // namespace okj
