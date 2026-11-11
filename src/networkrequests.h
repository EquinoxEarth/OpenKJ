#ifndef OPENKJ_NETWORKREQUESTS_H
#define OPENKJ_NETWORKREQUESTS_H

#include <QJsonDocument>
#include <QNetworkReply>
#include <QTimer>
#include <memory>
#include <utility>

namespace okj {

inline QNetworkRequest httpsRequest(const QUrl &url,
                                    QNetworkRequest::RedirectPolicy redirects = QNetworkRequest::SameOriginRedirectPolicy)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, redirects);
    return request;
}

inline bool successfulResponse(QNetworkReply *reply)
{
    return reply->error() == QNetworkReply::NoError &&
           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
}

template <typename Handler>
void readJsonReply(QNetworkReply *reply, QObject *context, qsizetype maxBytes, Handler finished)
{
    auto body = std::make_shared<QByteArray>();
    reply->setReadBufferSize(64 * 1024);
    QObject::connect(reply, &QNetworkReply::readyRead, context, [reply, body, maxBytes] {
        if (reply->bytesAvailable() > maxBytes - body->size()) {
            reply->abort();
            return;
        }
        body->append(reply->readAll());
    });
    QObject::connect(reply, &QNetworkReply::finished, context,
                     [reply, body, maxBytes, finished = std::move(finished)] {
        reply->deleteLater();
        if (!successfulResponse(reply) || reply->bytesAvailable() > maxBytes - body->size()) {
            finished(QJsonDocument{});
            return;
        }
        body->append(reply->readAll());
        finished(QJsonDocument::fromJson(*body));
    });
    QTimer::singleShot(30000, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
}

} // namespace okj

#endif
