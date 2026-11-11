#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QNetworkAccessManager>
#include <QObject>
#include "settings.h"

class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    void checkForUpdates();

signals:
    void newVersionAvailable(const QString &version, const QUrl &releasePage, bool preview);

private:
    QNetworkAccessManager *m_network;
    Settings m_settings;
    bool m_checking{false};
};

#endif // UPDATECHECKER_H
