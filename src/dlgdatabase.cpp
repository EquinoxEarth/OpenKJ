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

#include "dlgdatabase.h"
#include "ui_dlgdatabase.h"
#include <QDebug>
#include <QFileDialog>
#include <QInputDialog>
#include <QSqlQuery>
#include <QMessageBox>
#include <QStandardPaths>

DlgDatabase::DlgDatabase(TableModelKaraokeSongs &dbModel, QWidget *parent) :
    QDialog(parent),
    m_dbModel(dbModel),
    ui(new Ui::DlgDatabase)
{
    ui->setupUi(this);
    sourcedirmodel = new TableModelKaraokeSourceDirs();
    sourcedirmodel->loadFromDB();
    ui->tableViewFolders->setModel(sourcedirmodel);
    ui->tableViewFolders->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tableViewFolders->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(ui->tableViewFolders->selectionModel(), &QItemSelectionModel::selectionChanged, this, &DlgDatabase::on_foldersSelectionChanged);
    updateButtonsState();
    customPatternsDlg = new DlgCustomPatterns(this);
    dbUpdateDlg = new DlgDbUpdate(this);
    dbUpdateDlg->setWindowModality(Qt::NonModal);
    m_cancelScanButton = new QPushButton(tr("Cancel Update"), dbUpdateDlg);
    dbUpdateDlg->layout()->addWidget(m_cancelScanButton);
    connect(m_cancelScanButton, &QPushButton::clicked, this, &DlgDatabase::cancelScan);
    connect(dbUpdateDlg, &QDialog::rejected, this, &DlgDatabase::cancelScan);
    connect(this, &QDialog::rejected, this, [this] {
        if (m_manualScan)
            cancelScan();
    });
    connect(&m_scanWatcher, &QFutureWatcher<DbUpdater::ScanResult>::finished, this, &DlgDatabase::scanFinished);
    m_scanProgressTimer.setInterval(200);
    connect(&m_scanProgressTimer, &QTimer::timeout, this, &DlgDatabase::updateScanProgress);

    if (m_settings.dbDirectoryWatchEnabled()) {
        m_directoryMonitor = new DirectoryMonitor(this, sourcedirmodel->getSourceDirs());
        connect(m_directoryMonitor, &DirectoryMonitor::scanRequested, this, [this](const QStringList &paths) {
            for (const auto &path : paths)
                m_pendingWatchPaths.insert(path);
            scanPendingWatchPaths();
        });
    }
}

DlgDatabase::~DlgDatabase()
{
    cancelScan();
    delete m_directoryMonitor;
    delete sourcedirmodel;
    delete ui;
}

void DlgDatabase::singleSongAdd(const QString& path)
{
    qInfo() << "singleSongAdd(" << path << ") called";
    DbUpdater updater;
    if (!updater.addFilesToDatabase({path})) {
        qWarning() << "Could not add song to library:" << updater.getErrors();
        return;
    }
    QSqlQuery query;
    query.prepare("SELECT songid FROM dbsongs WHERE path=:path");
    query.bindValue(":path", path);
    if (query.exec() && query.next())
        emit songChanged(query.value(0).toInt());
}

void DlgDatabase::on_buttonNew_clicked()
{
#ifdef Q_OS_LINUX
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select a karaoke source dir",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog
            );
#else
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select a karaoke source dir",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            QFileDialog::ShowDirsOnly
            );
#endif
    if (fileName != "")
    {
        bool okPressed = false;
        QStringList items;
        QSqlQuery query;
        query.exec("SELECT * FROM custompatterns ORDER BY name");
        while (query.next())
        {
            QString name = query.value("name").toString();
            items << QString(tr("Custom: ") + name);
        }


        items << tr("SongID - Artist - Title") << tr("SongID - Title - Artist") << tr("Artist - Title - SongID") << tr("Title - Artist - SongID") << tr("Artist - Title") << tr("Title - Artist") << tr("SongID_Title_Artist") << tr("Media Tags");
        QString selected = QInputDialog::getItem(this,"Select a file naming pattern","Pattern",items,0,false,&okPressed);
        if (okPressed)
        {
            int pattern = 0;
            int customPattern = -1;
            if (selected == tr("SongID - Artist - Title")) pattern = SourceDir::SAT;
            if (selected == tr("SongID - Title - Artist")) pattern = SourceDir::STA;
            if (selected == tr("Artist - Title - SongID")) pattern = SourceDir::ATS;
            if (selected == tr("Title - Artist - SongID")) pattern = SourceDir::TAS;
            if (selected == tr("Artist - Title")) pattern = SourceDir::AT;
            if (selected == tr("Title - Artist")) pattern = SourceDir::TA;
            if (selected == tr("Media Tags")) pattern = SourceDir::METADATA;
            if (selected == tr("SongID_Title_Artist")) pattern = SourceDir::S_T_A;
            if (selected.contains(tr("Custom")))
            {
                pattern = SourceDir::CUSTOM;
                QString name = selected.split(": ").at(1);
                query.exec("SELECT patternid FROM custompatterns WHERE name == \"" + name + "\"");
                if (query.first())
                    customPattern = query.value(0).toInt();
            }
            sourcedirmodel->addSourceDir(fileName, pattern, customPattern);
        }
    }
    updateButtonsState();
}

void DlgDatabase::on_buttonClose_clicked()
{
    if (m_manualScan)
        cancelScan();
    m_settings.saveColumnWidths(ui->tableViewFolders);
    ui->tableViewFolders->clearSelection();
    hide();
}

void DlgDatabase::on_buttonDelete_clicked()
{
    int index = ui->tableViewFolders->currentIndex().row();
    if (index >= 0)
    {
        sourcedirmodel->delSourceDir(index);
        updateButtonsState();
    }
}

void DlgDatabase::on_buttonUpdate_clicked()
{
    scan(false);
}

void DlgDatabase::on_buttonUpdateAll_clicked()
{
    scan(true);
}

void DlgDatabase::scan(bool scanAllPaths)
{
    if (m_scanState)
        return;
    QStringList paths;
    DbUpdater::ProcessingOptions processingOptions = DbUpdater::ProcessingOption::PrepareForRemovalOfMissing;

    if (scanAllPaths) {
        processingOptions |= DbUpdater::ProcessingOption::FixMovedFilesSearchInWholeDB;
        for (int i=0; i < sourcedirmodel->size(); i++)
            paths.append(sourcedirmodel->getDirByIndex(i).getPath());
    }
    else {
        processingOptions |= DbUpdater::ProcessingOption::FixMovedFiles;
        int index = ui->tableViewFolders->currentIndex().row();
        if (index >= 0) {
            paths.append(sourcedirmodel->getDirByIndex(index).getPath());
        }
    }

    if (paths.isEmpty())
        return;

    m_scanAllPaths = scanAllPaths;
    startScan(paths, processingOptions, true);
}

void DlgDatabase::scanPendingWatchPaths()
{
    if (m_scanState || m_pendingWatchPaths.isEmpty())
        return;
    const auto paths = m_pendingWatchPaths.values();
    m_pendingWatchPaths.clear();
    startScan(paths, DbUpdater::FixMovedFiles | DbUpdater::SkipUnavailableSources, false);
}

void DlgDatabase::startScan(const QStringList &paths, DbUpdater::ProcessingOptions options, bool manual)
{
    m_scanPaths = paths;
    m_scanWarnings.clear();
    m_manualScan = manual;
    m_removingMissing = false;
    m_scanState = std::make_shared<DbUpdater::ScanState>();
    updateButtonsState();
    if (manual)
        showScanProgress();
    m_scanWatcher.setFuture(DbUpdater::scanAsync(QSqlDatabase::database().databaseName(), paths, options, m_scanState));
}

void DlgDatabase::showScanProgress()
{
    dbUpdateDlg->reset();
    dbUpdateDlg->changeStatusTxt(tr("Starting database update…"));
    dbUpdateDlg->changeProgress(0, 0);
    m_cancelScanButton->setEnabled(true);
    dbUpdateDlg->show();
    m_scanProgressTimer.start();
}

void DlgDatabase::cancelScan()
{
    if (!m_scanState)
        return;
    m_scanState->cancelled.store(true);
    m_cancelScanButton->setEnabled(false);
    dbUpdateDlg->changeStatusTxt(tr("Cancelling database update…"));
}

void DlgDatabase::updateScanProgress()
{
    if (!m_scanState || m_scanState->cancelled.load())
        return;
    DbUpdater::Progress progress;
    {
        QMutexLocker lock(&m_scanState->mutex);
        progress = m_scanState->progress;
    }
    if (!progress.status.isEmpty()) {
        dbUpdateDlg->changeStatusTxt(progress.status);
        dbUpdateDlg->changeProgress(progress.value, progress.maximum);
    }
}

void DlgDatabase::scanFinished()
{
    const auto result = m_scanWatcher.result();
    m_scanProgressTimer.stop();
    dbUpdateDlg->hide();

    if (m_manualScan && result.success && !result.missing.isEmpty() && !m_scanState->cancelled.load()) {
        m_scanWarnings = result.errors;
        confirmMissingFiles(result.missing);
        return;
    }
    finishScan(result);
}

void DlgDatabase::confirmMissingFiles(const QList<DbUpdater::MissingFile> &files)
{
    auto *dialog = new QMessageBox(QMessageBox::Question, tr("Database update"),
                                   tr("Remove missing files from database?"),
                                   QMessageBox::Yes | QMessageBox::No, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setDefaultButton(QMessageBox::No);
    QString text = tr("%1 file(s) in the database were missing during the scan. Do you want to remove them?").arg(files.size());
    if (!m_scanAllPaths)
        text += tr("\n\nChoose No and run Update All to check whether the files moved to another source folder.");
    dialog->setInformativeText(text);
    if (!m_scanWarnings.isEmpty())
        dialog->setDetailedText(m_scanWarnings.join("\n"));
    connect(dialog, &QMessageBox::finished, this, [this, files](int answer) {
        if (answer != QMessageBox::Yes || m_scanState->cancelled.load()) {
            finishScan({.success = true});
            return;
        }
        m_removingMissing = true;
        m_scanState = std::make_shared<DbUpdater::ScanState>();
        showScanProgress();
        m_scanWatcher.setFuture(DbUpdater::removeMissingAsync(QSqlDatabase::database().databaseName(),
                                                              m_scanPaths, files, m_scanState));
    });
    dialog->open();
}

void DlgDatabase::finishScan(const DbUpdater::ScanResult &result)
{
    if (result.success || m_removingMissing)
        emit databaseUpdateComplete();

    const QStringList errors = m_scanWarnings + result.errors;
    if (m_manualScan && isVisible()) {
        QString title = tr("Update Failed");
        QString summary = tr("Could not complete the database update.");
        if (result.cancelled) {
            title = tr("Update Cancelled");
            summary = m_removingMissing ? tr("The library scan finished. Removal of missing files was cancelled.")
                                        : tr("The library scan was cancelled.");
        } else if (result.success) {
            title = tr("Update Complete");
            summary = errors.isEmpty() ? tr("Database update complete.")
                                       : tr("Database update complete. Some files were skipped.");
        }
        auto *dialog = new QMessageBox(result.success || result.cancelled ? QMessageBox::Information : QMessageBox::Warning,
                                       title, summary, QMessageBox::Ok, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        if (!errors.isEmpty())
            dialog->setDetailedText(errors.join("\n"));
        dialog->open();
    } else if (!errors.isEmpty()) {
        qWarning() << "Karaoke library scan:" << errors;
    }

    m_scanState.reset();
    updateButtonsState();
    scanPendingWatchPaths();
}

void DlgDatabase::on_btnClearDatabase_clicked()
{
    QMessageBox msgBox;
    msgBox.setText(tr("Are you sure?"));
    msgBox.setInformativeText(tr("Clearing the song database will also clear the rotation and all saved regular singer data.  If you have not already done so, you may want to export your regular singers before performing this operation.  This operation can not be undone."));
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.addButton(QMessageBox::Cancel);
    QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
    msgBox.exec();
    if (msgBox.clickedButton() == yesButton) {
        QSqlQuery query;
        query.exec("DELETE FROM dbSongs");
        query.exec("DELETE FROM regularsongs");
        query.exec("DELETE FROM regularsingers");
        query.exec("DELETE FROM queuesongs");
        query.exec("DELETE FROM rotationsingers");
        emit databaseCleared();
        QMessageBox::information(this, tr("Database cleared"), tr("Song database, regular singers, and all rotation data has been cleared."));
    }
}

void DlgDatabase::on_btnCustomPatterns_clicked()
{
    customPatternsDlg->show();
}

void DlgDatabase::on_btnExport_clicked()
{
    QString defaultFilePath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QDir::separator() + "dbexport.csv";
    qDebug() << "Default save location: " << defaultFilePath;
#ifdef Q_OS_LINUX
    QString saveFilePath = QFileDialog::getSaveFileName(this,tr("Select DB export filename"), defaultFilePath, "(*.csv)",
                                                        nullptr, QFileDialog::DontUseNativeDialog);
#else
    QString saveFilePath = QFileDialog::getSaveFileName(this,tr("Select DB export filename"), defaultFilePath, "(*.csv)",
                                                        nullptr);
#endif
    if (saveFilePath != "")
    {
        QFile csvFile(saveFilePath);
        if (!csvFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QMessageBox::warning(this, tr("Error saving file"), tr("Unable to open selected file for writing.  Please verify that you have the proper permissions to write to that location."),QMessageBox::Close);
            return;
        }
        QSqlQuery query;
        query.exec("SELECT * from dbsongs ORDER BY artist,title,filename");
        while (query.next())
        {
            QString artist = query.value("artist").toString();
            QString title = query.value("title").toString();
            QString songId = query.value("discid").toString();
            QString filepath = query.value("path").toString();
            QString data = "\"" + artist + "\",\"" + title + "\",\"" + songId + "\",\"" + filepath + "\"" + "\n";
            csvFile.write(data.toLocal8Bit().data());
        }
        csvFile.close();
    }
}

void DlgDatabase::on_foldersSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    updateButtonsState();
}

void DlgDatabase::updateButtonsState()
{
    ui->groupBox->setEnabled(!m_scanState);
    bool hasSelectedRow = ui->tableViewFolders->selectionModel()->selectedRows().count() > 0;
    ui->buttonUpdate->setEnabled(hasSelectedRow);
    ui->buttonDelete->setEnabled(hasSelectedRow);

    auto model = ui->tableViewFolders->model();
    ui->buttonUpdateAll->setEnabled(model && model->rowCount() > 0);
}

