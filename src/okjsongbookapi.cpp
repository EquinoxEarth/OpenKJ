#include "okjsongbookapi.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QCoreApplication>
#include <QPointer>
#include <limits>
#include <cmath>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSqlDatabase>
#include "idledetect.h"

extern IdleDetect *filter;

namespace {
constexpr qint64 maxReplyBytes = 4 * 1024 * 1024;

QString replyText(const char *message)
{
    return QCoreApplication::translate("OKJSongbookAPI", message);
}

QNetworkReply *sendRequest(QNetworkAccessManager &network, const QUrl &url, const QJsonObject &payload,
                           bool ignoreCertificates, int deadlineMs,
                           std::function<void(QNetworkReply *, QJsonObject, QString)> finished,
                           std::function<void()> sslFailed = {})
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    auto *reply = network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setReadBufferSize(maxReplyBytes + 1);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply, ignoreCertificates, sslFailed] {
        if (ignoreCertificates)
            reply->ignoreSslErrors();
        else if (sslFailed)
            sslFailed();
    });
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > maxReplyBytes) {
            reply->setProperty("songbookError", replyText("The Songbook reply was too large."));
            reply->abort();
        }
    });
    QTimer::singleShot(deadlineMs, Qt::PreciseTimer, reply, [reply] {
        if (!reply->isFinished()) {
            reply->setProperty("songbookError", replyText("The Songbook request timed out."));
            reply->abort();
        }
    });
    const QString command = payload.value("command").toString();
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, command, finished = std::move(finished)] {
        reply->deleteLater();
        QString error = reply->property("songbookError").toString();
        QJsonObject response;
        if (error.isEmpty()) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 0 && (status < 200 || status >= 300))
                error = replyText("The Songbook server returned HTTP %1.").arg(status);
            else if (reply->error() != QNetworkReply::NoError)
                error = reply->errorString();
            else if (status == 0 || reply->bytesAvailable() > maxReplyBytes)
                error = replyText("The Songbook server returned an invalid HTTP reply.");
            else {
                response = QJsonDocument::fromJson(reply->readAll()).object();
                const auto flag = response.value("error");
                const auto flagText = flag.toString().toLower();
                const auto success = response.value("success");
                const auto errors = response.value("errors");
                const auto echo = response.value("command");
                error = response.value("errorString").toString().trimmed().left(500);
                if (error.isEmpty() && (flag.toBool() || flagText == "true" ||
                    (success.isBool() && !success.toBool()) ||
                    (!errors.isUndefined() && !errors.isNull() && (!errors.isArray() || !errors.toArray().isEmpty()))))
                    error = replyText("The Songbook server reported an error.");
                if (error.isEmpty() && (echo.toString() != command ||
                    (!flag.isUndefined() && !flag.isNull() && !flag.isBool() && flagText != "false")))
                    error = replyText("The Songbook server returned an unexpected reply.");
            }
        }
        finished(reply, response, error);
    });
    return reply;
}

bool readInteger(const QJsonValue &value, int &result)
{
    if (value.isString()) {
        bool ok;
        result = value.toString().toInt(&ok);
        return ok;
    }
    const auto number = value.toDouble();
    if (!value.isDouble() || number != std::trunc(number) ||
        number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        return false;
    result = static_cast<int>(number);
    return true;
}

bool readBoolean(const QJsonValue &value, bool &result)
{
    if (value.isBool()) {
        result = value.toBool();
        return true;
    }
    const auto flag = value.toString().toLower();
    if (flag == "true" || flag == "false") {
        result = flag == "true";
        return true;
    }
    int number;
    if (!readInteger(value, number) || (number != 0 && number != 1))
        return false;
    result = number == 1;
    return true;
}
}

OKJSongbookAPI::OKJSongbookAPI(QObject *parent, QNetworkAccessManager *network, int requestDeadlineMs)
    : QObject(parent), m_logger(spdlog::get("logger")), serial(0), entitledSystems(1),
      programIsIdle(false), configuration(currentConfiguration()), deadlineMs(requestDeadlineMs)
{
    delayErrorEmitted = false;
    connectionReset = false;
    syncAge.start();
    timer = new QTimer(this);
    timer->setInterval(m_settings.requestServerInterval() * 1000);
    alertTimer = new QTimer(this);
    alertTimer->start(600000);
    manager = network ? network : new QNetworkAccessManager(this);
    connect(&m_songDbUpload, &okj::SongbookUpload::prepared, this, &OKJSongbookAPI::remoteSongDbUpdatePrepared);
    connect(&m_songDbUpload, &okj::SongbookUpload::progress, this, &OKJSongbookAPI::remoteSongDbUpdateProgress);
    connect(&m_songDbUpload, &okj::SongbookUpload::finished, this, &OKJSongbookAPI::remoteSongDbUpdateFinished);
    connect(timer, &QTimer::timeout, this, &OKJSongbookAPI::timerTimeout);
    connect(alertTimer, &QTimer::timeout, this, &OKJSongbookAPI::alertTimerTimeout);
    if (filter)
        connect(filter, &IdleDetect::idleStateChanged, this, &OKJSongbookAPI::idleStateChanged);
    if (configuration.enabled) {
        getEntitledSystemCount();
        refreshVenues();
        alertCheck();
    }
    timer->start();
}

OKJSongbookAPI::Configuration OKJSongbookAPI::currentConfiguration()
{
    return {QUrl(m_settings.requestServerUrl()), m_settings.requestServerApiKey(),
            m_settings.requestServerVenue(), m_settings.systemId(), m_settings.requestServerEnabled(),
            m_settings.requestServerIgnoreCertErrors()};
}

void OKJSongbookAPI::configurationChanged()
{
    const auto next = currentConfiguration();
    if (next == configuration)
        return;
    const bool accountChanged = next.url != configuration.url || next.apiKey != configuration.apiKey ||
                                next.enabled != configuration.enabled;
    const bool requestSourceChanged = accountChanged || next.venue != configuration.venue;
    if (accountChanged) {
        venues.clear();
        venuesLoaded = false;
    }
    configuration = next;
    ++generation;
    serial = 0;
    sslErrorReported = false;
    syncAge.restart();
    delayErrorEmitted = false;
    connectionReset = false;
    polls.clear();
    repeatPolls.clear();
    const auto obsolete = activeReplies;
    for (auto *reply : obsolete)
        reply->abort();
    if (requestSourceChanged && !requests.isEmpty()) {
        requests.clear();
        emit requestsChanged(requests);
    }
}

void OKJSongbookAPI::post(const QString &command, QJsonObject payload, bool poll, bool repeat)
{
    configurationChanged();
    if (!configuration.enabled)
        return;
    if (poll && polls.contains(command)) {
        if (repeat)
            repeatPolls.insert(command);
        return;
    }
    if (poll)
        polls.insert(command);

    payload.insert("command", command);
    if (command != "testingAddRandomRequest")
        payload.insert("api_key", configuration.apiKey);

    auto context = QPointer<OKJSongbookAPI>(this);
    const auto issuedGeneration = generation;
    auto *reply = sendRequest(*manager, configuration.url, payload, configuration.ignoreCertificates, deadlineMs,
        [context, issuedGeneration, command, payload, poll](QNetworkReply *reply, const QJsonObject &response, const QString &error) {
            if (!context)
                return;
            context->activeReplies.remove(reply);
            context->configurationChanged();
            if (issuedGeneration != context->generation)
                return;
            if (poll)
                context->polls.remove(command);
            const bool repeat = context->repeatPolls.remove(command);
            if (!error.isEmpty()) {
                if (context->m_logger)
                    context->m_logger->warn("{} {} failed: {}", context->m_loggingPrefix, command.toStdString(), error.toStdString());
                if (!poll)
                    emit context->requestActionFailed(
                        context->tr("The Songbook action could not be confirmed. %1").arg(error));
                else if (command == "getRequests" || command == "getVenues")
                    context->serial = 0;
            } else if (!repeat) {
                context->handleReply(command, response);
            }
            if (context && repeat && issuedGeneration == context->generation)
                context->post(command, payload, true);
        },
        [context, issuedGeneration] {
            if (!context || issuedGeneration != context->generation ||
                context->configuration != context->currentConfiguration() || context->sslErrorReported)
                return;
            context->sslErrorReported = true;
            emit context->sslError();
        });
    activeReplies.insert(reply);
}

void OKJSongbookAPI::getSerial()
{
    post("getSerial", {}, true);
}

void OKJSongbookAPI::refreshRequests()
{
    post("getRequests", {{"venue_id", m_settings.requestServerVenue()}}, true);
}

void OKJSongbookAPI::triggerTestAdd()
{
    post("testingAddRandomRequest", {{"venue_id", m_settings.requestServerVenue()}}, false);
}

void OKJSongbookAPI::removeRequest(int requestId)
{
    post("deleteRequest", {{"venue_id", m_settings.requestServerVenue()}, {"request_id", requestId}}, false);
}

bool OKJSongbookAPI::getAccepting()
{
    for (const auto &venue : venues)
        if (venue.venueId == m_settings.requestServerVenue())
            return venue.accepting;
    return false;
}

void OKJSongbookAPI::setAccepting(bool enabled)
{
    alertCheck();
    post("setAccepting", {{"venue_id", m_settings.requestServerVenue()}, {"accepting", enabled},
                          {"system_id", m_settings.systemId()}}, false);
}

void OKJSongbookAPI::refreshVenues()
{
    post("getVenues", {}, true);
}

void OKJSongbookAPI::clearRequests()
{
    post("clearRequests", {{"venue_id", m_settings.requestServerVenue()}}, false);
}

void OKJSongbookAPI::updateSongDb()
{
    const auto database = QSqlDatabase::database(QLatin1String(QSqlDatabase::defaultConnection), false);
    m_songDbUpload.start({database.databaseName(), QUrl(m_settings.requestServerUrl()),
                          m_settings.requestServerApiKey(), m_settings.systemId(),
                          m_settings.requestServerIgnoreCertErrors()});
}

void OKJSongbookAPI::dbUpdateCanceled()
{
    m_songDbUpload.cancel();
}

void OKJSongbookAPI::testConnection(QNetworkAccessManager &network, const QUrl &url, const QString &apiKey,
                                    bool ignoreCertificates, std::function<void(QString)> finished, int deadlineMs)
{
    sendRequest(network, url, {{"api_key", apiKey}, {"command", "getSerial"}}, ignoreCertificates, deadlineMs,
        [finished = std::move(finished)](QNetworkReply *, const QJsonObject &response, QString error) {
            int serial;
            if (error.isEmpty() && (!readInteger(response.value("serial"), serial) || serial == 0))
                error = replyText("The Songbook server did not return a valid serial.");
            finished(error);
        });
}

void OKJSongbookAPI::alertCheck()
{
    post("getAlert", {}, true);
}

void OKJSongbookAPI::refreshLists()
{
    post("getRequests", {{"venue_id", m_settings.requestServerVenue()}}, true, true);
    post("getVenues", {}, true, true);
}

void OKJSongbookAPI::markSynchronized()
{
    syncAge.restart();
    delayErrorEmitted = false;
    connectionReset = false;
    emit synchronized(QTime::currentTime());
}

void OKJSongbookAPI::handleReply(const QString &command, const QJsonObject &response)
{
    if (command == "getSerial") {
        int next;
        if (!readInteger(response.value("serial"), next) || next == 0)
            return;
        if (serial != next) {
            serial = next;
            refreshLists();
        }
        markSynchronized();
    } else if (command == "getRequests") {
        const auto list = response.value("requests");
        if (!list.isArray()) {
            serial = 0;
            return;
        }
        OkjsRequests next;
        QSet<int> ids;
        for (const auto &entry : list.toArray()) {
            const auto row = entry.toObject();
            OkjsRequest request;
            if (!readInteger(row.value("request_id"), request.requestId) || request.requestId <= 0 ||
                ids.contains(request.requestId) || !row.value("artist").isString() ||
                !row.value("title").isString() || !row.value("singer").isString() ||
                !readInteger(row.value("request_time"), request.time) ||
                !readInteger(row.value("key_change"), request.key)) {
                serial = 0;
                return;
            }
            ids.insert(request.requestId);
            request.artist = row.value("artist").toString();
            request.title = row.value("title").toString();
            request.singer = row.value("singer").toString();
            next.append(request);
        }
        if (requests != next) {
            requests = next;
            emit requestsChanged(requests);
        }
        markSynchronized();
    } else if (command == "getVenues") {
        const auto list = response.value("venues");
        if (!list.isArray()) {
            serial = 0;
            return;
        }
        OkjsVenues next;
        QSet<int> ids;
        for (const auto &entry : list.toArray()) {
            const auto row = entry.toObject();
            OkjsVenue venue;
            if (!readInteger(row.value("venue_id"), venue.venueId) || venue.venueId <= 0 ||
                ids.contains(venue.venueId) || !row.value("name").isString() ||
                !row.value("url_name").isString() || !readBoolean(row.value("accepting"), venue.accepting)) {
                serial = 0;
                return;
            }
            ids.insert(venue.venueId);
            venue.name = row.value("name").toString();
            venue.urlName = row.value("url_name").toString();
            next.append(venue);
        }
        if (!venuesLoaded || venues != next) {
            venuesLoaded = true;
            venues = next;
            const auto replyGeneration = generation;
            emit venuesChanged(venues);
            if (replyGeneration != generation)
                return;
            getEntitledSystemCount();
        }
        markSynchronized();
    } else if (command == "getEntitledSystemCount") {
        int count;
        if (readInteger(response.value("count"), count) && count >= 1) {
            entitledSystems = count;
            emit entitledSystemCountChanged(count);
        }
    } else if (command == "getAlert") {
        bool alert;
        if (readBoolean(response.value("alert"), alert) && alert &&
            response.value("title").isString() && response.value("message").isString()) {
            emit alertRecieved(response.value("title").toString(), response.value("message").toString());
            refreshVenues();
        }
    } else {
        refreshLists();
    }
}

void OKJSongbookAPI::timerTimeout()
{
    configurationChanged();
    if (!configuration.enabled || programIsIdle)
        return;

    const int seconds = static_cast<int>(syncAge.elapsed() / 1000);
    if (seconds > 300 && !delayErrorEmitted) {
        delayErrorEmitted = true;
        emit delayError(seconds);
    }
    if (seconds > 200 && !connectionReset) {
        connectionReset = true;
        refreshRequests();
        refreshVenues();
    }
    getSerial();
}

void OKJSongbookAPI::alertTimerTimeout()
{
    alertCheck();
}

void OKJSongbookAPI::setInterval(int interval)
{
    timer->setInterval(interval * 1000);
}

void OKJSongbookAPI::idleStateChanged(bool isIdle)
{
    programIsIdle = isIdle;
    if (!isIdle) {
        syncAge.restart();
        timerTimeout();
    }
}

void OKJSongbookAPI::getEntitledSystemCount()
{
    post("getEntitledSystemCount", {}, true);
}

bool OkjsVenue::operator ==(const OkjsVenue &v) const
{
    if (venueId != v.venueId)
        return false;
    if (name != v.name)
        return false;
    if (urlName != v.urlName)
        return false;
    if (accepting != v.accepting)
        return false;
    return true;
}

bool OkjsRequest::operator ==(const OkjsRequest& r) const
{
    if (r.requestId != requestId)
        return false;
    if (r.artist != artist)
        return false;
    if (r.title != title)
        return false;
    if (r.key != key)
        return false;
    if (r.time != time)
        return false;
    if (r.singer != singer)
        return false;
    return true;
}

