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

#ifndef DATABASEDIALOG_H
#define DATABASEDIALOG_H

#include <QDialog>
#include <QDir>
#include "src/models/tablemodelkaraokesourcedirs.h"
#include "dlgcustompatterns.h"
#include <QSqlDatabase>
#include "dlgdbupdate.h"
#include "models/tablemodelkaraokesongs.h"
#include "directorymonitor.h"
#include "settings.h"
#include "dbupdater.h"
#include <QFutureWatcher>

namespace Ui {
class DlgDatabase;
}

class DlgDatabase : public QDialog
{
    Q_OBJECT
    
private:
    Ui::DlgDatabase *ui;
    TableModelKaraokeSourceDirs *sourcedirmodel;
    DlgCustomPatterns *customPatternsDlg;
    TableModelKaraokeSongs &m_dbModel;
    DlgDbUpdate *dbUpdateDlg;
    Settings m_settings;
    DirectoryMonitor *m_directoryMonitor {nullptr};

    QFutureWatcher<DbUpdater::ScanResult> m_scanWatcher;
    std::shared_ptr<DbUpdater::ScanState> m_scanState;
    QTimer m_scanProgressTimer;
    QPushButton *m_cancelScanButton;
    QSet<QString> m_pendingWatchPaths;
    QStringList m_scanPaths;
    QStringList m_scanWarnings;
    bool m_manualScan{false};
    bool m_scanAllPaths{false};
    bool m_removingMissing{false};

    void scan(bool scanAllPaths);
    void startScan(const QStringList &paths, DbUpdater::ProcessingOptions options, bool manual);
    void scanPendingWatchPaths();
    void showScanProgress();
    void updateScanProgress();
    void scanFinished();
    void finishScan(const DbUpdater::ScanResult &result);
    void confirmMissingFiles(const QList<DbUpdater::MissingFile> &files);
    void updateButtonsState();

public:
    explicit DlgDatabase(TableModelKaraokeSongs &dbModel, QWidget *parent = nullptr);
    ~DlgDatabase() override;

signals:
    void databaseAboutToUpdate();
    void databaseUpdateComplete();
    void databaseCleared();
    void databaseSongAdded();
    void songChanged(int songId);

public slots:
    void singleSongAdd(const QString &path);
    void cancelScan();

private slots:
    void on_buttonUpdateAll_clicked();
    void on_buttonNew_clicked();
    void on_buttonClose_clicked();
    void on_buttonDelete_clicked();
    void on_buttonUpdate_clicked();
    void on_btnClearDatabase_clicked();
    void on_btnCustomPatterns_clicked();
    void on_btnExport_clicked();
    void on_foldersSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
};

#endif // DATABASEDIALOG_H
