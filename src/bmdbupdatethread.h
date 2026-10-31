/*
 * Copyright (c) 2013-2019 Thomas Isaac Lightburn
 *
 *
 * This file is part of OpenKJ.
 *
 * OpenKJ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef BMDBUPDATETHREAD_H
#define BMDBUPDATETHREAD_H

#include <QFutureWatcher>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>

struct BmDbUpdateResult {
    QString error;
    QStringList unavailablePaths;
    qint64 processed = 0, total = 0, added = 0, existing = 0, unavailableFiles = 0;
    bool cancelled = false;
};
Q_DECLARE_METATYPE(BmDbUpdateResult)

class BmDbUpdater : public QObject {
    Q_OBJECT
public:
    explicit BmDbUpdater(QObject *parent = nullptr);
    ~BmDbUpdater() override;
    bool start(const QString &databasePath, const QStringList &paths);
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void progressMessage(const QString &message);
    void stateChanged(const QString &state);
    void progressChanged(qint64 processed, qint64 total);
    void finished(const BmDbUpdateResult &result);

private:
    struct State {
        std::atomic_bool cancelled{false};
        std::atomic<qint64> processed{0}, total{0};
        QMutex mutex;
        QString status, message;
    };
    static BmDbUpdateResult run(const QString &databasePath, const QStringList &paths,
                               const std::shared_ptr<State> &state);
    void reportProgress();
    QFutureWatcher<BmDbUpdateResult> m_watcher{this};
    QTimer m_progressTimer{this};
    std::shared_ptr<State> m_state;
    QString m_lastStatus, m_lastMessage;
    bool m_running = false;
};

#endif
