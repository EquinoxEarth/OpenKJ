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

#include "dlgregularimport.h"
#include "regularpathmapping.h"
#include "ui_dlgregularimport.h"

#include <QFileDialog>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUuid>
#include <QtConcurrentRun>

DlgRegularImport::DlgRegularImport(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::DlgRegularImport)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    m_rootGroup = new QGroupBox(tr("Song folders"), this);
    auto *layout = new QVBoxLayout(m_rootGroup);
    auto *help = new QLabel(tr("Choose where each exported folder lives on this computer. "
                              "Unavailable songs keep their history and are reported after import."));
    help->setWordWrap(true);
    layout->addWidget(help);
    m_rootTable = new QTableWidget(0, 3, m_rootGroup);
    m_rootTable->setObjectName("tableRootMappings");
    m_rootTable->setHorizontalHeaderLabels({tr("Exported root / old folder"), tr("Folder on this computer"), tr("Choose")});
    m_rootTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_rootTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_rootTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(m_rootTable);
    auto *add = new QPushButton(tr("Add old folder mapping"), m_rootGroup);
    layout->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString source = QInputDialog::getText(this, tr("Old song folder"),
            tr("Folder prefix in the export (for example /Volumes/Karaoke Drive):"),
            QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || source.isEmpty()) return;
        const QString clean = RegularPaths::normalized(source);
        if (!RegularPaths::absolute(clean)) {
            QMessageBox::warning(this, tr("Invalid folder"), tr("Enter an absolute folder path."));
            return;
        }
        for (int row = 0; row < m_rootTable->rowCount(); ++row)
            if (m_rootTable->item(row, 0)->text() == clean) return;
        addRootMapping(clean, clean);
    });
    ui->verticalLayout->insertWidget(1, m_rootGroup);
    m_rootGroup->hide();
    ui->pushButtonImport->setEnabled(false);
    ui->pushButtonImportAll->setEnabled(false);
    resize(780, 600);
}

DlgRegularImport::~DlgRegularImport()
{
    delete ui;
}

void DlgRegularImport::on_pushButtonSelectFile_clicked()
{
#ifdef Q_OS_LINUX
    QString importFile = QFileDialog::getOpenFileName(this,tr("Select file to load regulars from"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), "OpenKJ export files (*.xml *.json)",
                                                      nullptr, QFileDialog::DontUseNativeDialog);
#else
    QString importFile = QFileDialog::getOpenFileName(this,tr("Select file to load regulars from"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), "OpenKJ export files (*.xml *.json)",
                                                      nullptr);
#endif
    if (importFile.isEmpty())
        return;

    setBusy(true);
    auto *watcher = new QFutureWatcher<RegularsImport::Export>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        auto loaded = watcher->result();
        watcher->deleteLater();
        if (!loaded.error.isEmpty()) {
            QMessageBox::warning(this, tr("Cannot load export"), loaded.error);
        } else {
            m_export = std::move(loaded);
            m_rootTable->setRowCount(0);
            m_rootGroup->setVisible(!m_export.legacy);
            for (auto it = m_export.roots.cbegin(); it != m_export.roots.cend(); ++it)
                addRootMapping(it.key(), it.value(), it.value());

            ui->listWidgetRegulars->clear();
            for (qsizetype index = 0; index < m_export.singers.size(); ++index) {
                auto *item = new QListWidgetItem(m_export.singers.at(index).name, ui->listWidgetRegulars);
                item->setData(Qt::UserRole, index);
            }
            ui->listWidgetRegulars->sortItems();
        }
        setBusy(false);
    });
    watcher->setFuture(QtConcurrent::run(RegularsImport::readExport, importFile));
}

void DlgRegularImport::on_pushButtonClose_clicked()
{
    close();
}

void DlgRegularImport::on_pushButtonImport_clicked()
{
    const auto items = ui->listWidgetRegulars->selectedItems();
    if (items.isEmpty())
        return;

    QMap<QString, QString> mappings;
    if (!m_export.legacy && !readMappings(mappings))
        return;

    QHash<QString, int> existingIds;
    QSqlQuery query;
    if (query.exec("SELECT name, id FROM historySingers")) {
        while (query.next())
            existingIds.insert(query.value(0).toString().toCaseFolded(), query.value(1).toInt());
    }
    if (query.lastError().isValid()) {
        QMessageBox::critical(this, tr("Import failed"), query.lastError().text());
        return;
    }
    query.finish();

    QList<RegularsImport::Singer> selected;
    for (const auto *item : items) {
        auto singer = m_export.singers.at(item->data(Qt::UserRole).toLongLong());
        singer.existingId = existingIds.value(singer.name.toCaseFolded(), -1);
        if (singer.existingId != -1) {
            QMessageBox prompt(this);
            auto *merge = prompt.addButton(tr("Merge"), QMessageBox::AcceptRole);
            auto *replace = prompt.addButton(tr("Replace"), QMessageBox::DestructiveRole);
            auto *skip = prompt.addButton(tr("Skip"), QMessageBox::RejectRole);
            prompt.setDefaultButton(merge);
            prompt.setEscapeButton(skip);
            prompt.setWindowTitle(tr("Naming conflict"));
            prompt.setText(tr("A singer named %1 already exists. How would you like to proceed?").arg(singer.name));
            prompt.exec();
            if (prompt.clickedButton() != merge && prompt.clickedButton() != replace)
                continue;
            singer.replace = prompt.clickedButton() == replace;
        }
        selected.append(std::move(singer));
    }
    if (selected.isEmpty())
        return;

    setBusy(true);
    auto *watcher = new QFutureWatcher<RegularsImport::Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.error.isEmpty()) {
            QMessageBox::critical(this, tr("Import failed"), result.error);
        } else {
            if (result.imported > 0)
                emit historyImported();
            ui->listWidgetRegulars->clearSelection();
            if (!result.warnings.isEmpty()) {
                QMessageBox message(QMessageBox::Warning, tr("Regular singer import"),
                    result.imported > 0
                        ? tr("Import complete. Some songs were unavailable or could not be matched. See Details.")
                        : tr("No singers were imported. Some songs were unavailable or could not be matched. See Details."),
                    QMessageBox::Ok, this);
                message.setDetailedText(result.warnings.join('\n'));
                message.exec();
            } else {
                QMessageBox::information(this, tr("Import complete"), tr("Regular singer import complete."));
            }
        }
        setBusy(false);
    });

    const auto database = QSqlDatabase::database();
    watcher->setFuture(QtConcurrent::run([source = database.connectionName(), selected = std::move(selected),
                                         mappings = std::move(mappings), legacy = m_export.legacy]() mutable {
        const QString name = QUuid::createUuid().toString();
        const auto removeConnection = qScopeGuard([&name] { QSqlDatabase::removeDatabase(name); });
        auto workerDatabase = QSqlDatabase::cloneDatabase(source, name);
        if (!workerDatabase.open())
            return RegularsImport::Result{workerDatabase.lastError().text(), {}, 0};
        return RegularsImport::importSingers(workerDatabase, std::move(selected), mappings, legacy);
    }));
}

void DlgRegularImport::on_pushButtonImportAll_clicked()
{
    ui->listWidgetRegulars->selectAll();
    on_pushButtonImport_clicked();
}

void DlgRegularImport::addRootMapping(const QString &source, const QString &destination, const QString &original)
{
    const int row = m_rootTable->rowCount();
    m_rootTable->insertRow(row);
    auto *sourceItem = new QTableWidgetItem(source);
    sourceItem->setFlags(sourceItem->flags() & ~Qt::ItemIsEditable);
    sourceItem->setToolTip(original.isEmpty() ? source : original);
    m_rootTable->setItem(row, 0, sourceItem);
    m_rootTable->setItem(row, 1, new QTableWidgetItem(destination));
    auto *browse = new QPushButton(tr("Browse…"));
    m_rootTable->setCellWidget(row, 2, browse);
    connect(browse, &QPushButton::clicked, this, [this, row]() {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Choose song folder"),
            m_rootTable->item(row, 1)->text());
        if (!folder.isEmpty()) m_rootTable->item(row, 1)->setText(folder);
    });
}

bool DlgRegularImport::readMappings(QMap<QString, QString> &mappings)
{
    for (int row = 0; row < m_rootTable->rowCount(); ++row) {
        const QString destination = m_rootTable->item(row, 1)->text().trimmed();
        if (destination.isEmpty() || !RegularPaths::absolute(RegularPaths::normalized(destination))) {
            QMessageBox::warning(this, tr("Choose a folder"), tr("Each root needs an absolute destination folder."));
            return false;
        }
        mappings.insert(m_rootTable->item(row, 0)->text(), destination);
    }
    return true;
}

void DlgRegularImport::setBusy(bool busy)
{
    m_busy = busy;
    ui->listWidgetRegulars->setEnabled(!busy);
    m_rootGroup->setEnabled(!busy);
    ui->pushButtonSelectFile->setEnabled(!busy);
    ui->pushButtonClose->setEnabled(!busy);
    ui->pushButtonImport->setEnabled(!busy && !m_export.singers.isEmpty());
    ui->pushButtonImportAll->setEnabled(!busy && !m_export.singers.isEmpty());
    setCursor(busy ? Qt::WaitCursor : Qt::ArrowCursor);
}

void DlgRegularImport::done(int result)
{
    if (!m_busy)
        QDialog::done(result);
}
