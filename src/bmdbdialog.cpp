#include "bmdbdialog.h"
#include "ui_bmdbdialog.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QSqlQuery>
#include <QLayout>
#include <QStandardPaths>
#include <algorithm>
#include <limits>

BmDbDialog::BmDbDialog(QWidget *parent) :
        QDialog(parent),
        ui(new Ui::BmDbDialog) {
    ui->setupUi(this);
    m_pathsModel.setTable("bmsrcdirs");
    m_pathsModel.select();
    ui->tableViewPaths->setModel(&m_pathsModel);
    m_pathsModel.sort(0, Qt::AscendingOrder);
    connect(ui->pushButtonAdd, &QPushButton::clicked, this, &BmDbDialog::pushButtonAddClicked);
    connect(ui->pushButtonClearDb, &QPushButton::clicked, this, &BmDbDialog::pushButtonClearDbClicked);
    connect(ui->pushButtonClose, &QPushButton::clicked, this, [this] {
        if (isUpdating())
            cancelUpdate();
        else
            close();
    });
    connect(ui->pushButtonDelete, &QPushButton::clicked, this, &BmDbDialog::pushButtonDeleteClicked);
    connect(ui->pushButtonUpdate, &QPushButton::clicked, this, &BmDbDialog::pushButtonUpdateClicked);
    connect(ui->pushButtonUpdateAll, &QPushButton::clicked, this, &BmDbDialog::pushButtonUpdateAllClicked);
    // Keep hosting controls available while the worker scans and reads tags.
    m_dbUpdateDlg.setWindowModality(Qt::NonModal);
    auto *cancel = new QPushButton(tr("Cancel Update"), &m_dbUpdateDlg);
    cancel->setObjectName("pushButtonCancelUpdate");
    m_dbUpdateDlg.layout()->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, &m_updater, &BmDbUpdater::cancel);
    connect(this, &QDialog::rejected, &m_updater, &BmDbUpdater::cancel);
    connect(&m_dbUpdateDlg, &QDialog::rejected, &m_updater, &BmDbUpdater::cancel);
    connect(&m_updater, &BmDbUpdater::progressMessage, &m_dbUpdateDlg, &DlgDbUpdate::addLogMsg);
    connect(&m_updater, &BmDbUpdater::stateChanged, &m_dbUpdateDlg, &DlgDbUpdate::changeStatusTxt);
    connect(&m_updater, &BmDbUpdater::progressChanged, &m_dbUpdateDlg, [this](qint64 done, qint64 total) {
        const auto limit = std::numeric_limits<int>::max();
        m_dbUpdateDlg.changeProgress(static_cast<int>(std::min<qint64>(done, limit)),
                                    static_cast<int>(std::min<qint64>(total, limit)));
    });
    connect(&m_updater, &BmDbUpdater::finished, this, [this](const BmDbUpdateResult &result) {
        m_dbUpdateDlg.hide();
        ui->groupBox->setEnabled(true);
        ui->pushButtonClose->setText(tr("Close"));
        // Completed batches remain valid on cancellation or a later batch error.
        emit bmDbUpdated();
        if (!isVisible())
            return;
        const auto summary = tr("%1 songs added; %2 already in the library.\n"
                                "%3 unavailable files and %4 unavailable folders skipped.")
            .arg(result.added).arg(result.existing).arg(result.unavailableFiles).arg(result.unavailablePaths.size());
        if (!result.error.isEmpty())
            QMessageBox::warning(this, tr("Update Failed"), result.error + "\n\n" + summary);
        else
            QMessageBox::information(this, result.cancelled ? tr("Update Cancelled") : tr("Update Complete"), summary);
    });

}

// This is here instead of the header to make moc & std::unique_ptr happy
BmDbDialog::~BmDbDialog()
{
    cancelUpdate();
}

void BmDbDialog::pushButtonAddClicked() {
    if (isUpdating() || !isEnabled())
        return;
#ifdef Q_OS_LINUX
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select a media source dir",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog
            );
#else
    QString fileName = QFileDialog::getExistingDirectory(
            this,
            "Select a media source dir",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            QFileDialog::ShowDirsOnly
            );
#endif
    if (fileName != "") {
        m_pathsModel.insertRow(m_pathsModel.rowCount());
        m_pathsModel.setData(m_pathsModel.index(m_pathsModel.rowCount() - 1, 0), fileName);
        m_pathsModel.submitAll();
    }
}


void BmDbDialog::startUpdate(const QStringList &paths)
{
    if (isUpdating() || !isEnabled() || paths.isEmpty())
        return;
    // Exhaust the model's SELECT so it cannot hold a read lock during writes.
    while (m_pathsModel.canFetchMore())
        m_pathsModel.fetchMore();
    m_dbUpdateDlg.reset();
    m_dbUpdateDlg.changeStatusTxt(tr("Finding media files…"));
    m_dbUpdateDlg.changeProgress(0, 0);
    ui->groupBox->setEnabled(false);
    ui->pushButtonClose->setText(tr("Cancel Update"));
    emit bmDbAboutToUpdate();
    m_updater.start(m_pathsModel.database().databaseName(), paths);
    m_dbUpdateDlg.show();
}

void BmDbDialog::pushButtonUpdateClicked()
{
    if (ui->tableViewPaths->selectionModel()->selectedIndexes().empty())
        return;
    const auto index = ui->tableViewPaths->selectionModel()->selectedIndexes().first();
    startUpdate({index.sibling(index.row(), 0).data().toString()});
}

void BmDbDialog::pushButtonUpdateAllClicked()
{
    if (isUpdating() || !isEnabled())
        return;
    while (m_pathsModel.canFetchMore())
        m_pathsModel.fetchMore();
    QStringList paths;
    for (int row = 0; row < m_pathsModel.rowCount(); ++row)
        paths.append(m_pathsModel.index(row, 0).data().toString());
    startUpdate(paths);
}

void BmDbDialog::pushButtonClearDbClicked() {
    if (isUpdating() || !isEnabled())
        return;
    QMessageBox msgBox(QMessageBox::Warning, "Are you sure?",
                       "Clearing the database will also clear all playlists.  If you have not already done so, "
                       "you may want to export your playlists before performing this operation.",
                       QMessageBox::Cancel | QMessageBox::Yes);
    if (msgBox.exec() != QMessageBox::Yes)
        return;
    QSqlQuery query;
    query.exec("DELETE FROM bmplaylists");
    query.exec("DELETE FROM bmplsongs");
    query.exec("DELETE FROM bmsongs");
    query.exec("UPDATE sqlite_sequence SET seq = 0");
    query.exec("VACUUM");
    m_pathsModel.select();
    emit bmDbCleared();
}

void BmDbDialog::pushButtonDeleteClicked() {
    if (isUpdating() || !isEnabled())
        return;
    if (ui->tableViewPaths->selectionModel()->selectedIndexes().empty())
        return;
    m_pathsModel.removeRow(ui->tableViewPaths->selectionModel()->selectedIndexes().at(0).row());
    m_pathsModel.select();
    m_pathsModel.submitAll();
}


