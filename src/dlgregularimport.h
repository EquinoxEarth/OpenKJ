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

#ifndef REGULARIMPORTDIALOG_H
#define REGULARIMPORTDIALOG_H

#include <QDialog>
#include "regularsimport.h"

class QTableWidget;
class QGroupBox;

namespace Ui {
class DlgRegularImport;
}

class DlgRegularImport : public QDialog
{
    Q_OBJECT

public:
    explicit DlgRegularImport(QWidget *parent = nullptr);
    ~DlgRegularImport();

signals:
    void historyImported();

protected:
    void done(int result) override;

private slots:
    void on_pushButtonSelectFile_clicked();
    void on_pushButtonClose_clicked();
    void on_pushButtonImport_clicked();
    void on_pushButtonImportAll_clicked();

private:
    void addRootMapping(const QString &source, const QString &destination, const QString &original = {});
    bool readMappings(QMap<QString, QString> &mappings);
    void setBusy(bool busy);

    Ui::DlgRegularImport *ui;
    RegularsImport::Export m_export;
    QTableWidget *m_rootTable;
    QGroupBox *m_rootGroup;
    bool m_busy = false;
};

#endif // REGULARIMPORTDIALOG_H
