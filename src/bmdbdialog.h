#ifndef BMDBDIALOG_H
#define BMDBDIALOG_H

#include <memory>
#include <QDialog>
#include <QSqlTableModel>
#include <QSqlDatabase>
#include "dlgdbupdate.h"
#include "bmdbupdatethread.h"


namespace Ui {
class BmDbDialog;
}

class BmDbDialog : public QDialog
{
    Q_OBJECT
    
public:
    explicit BmDbDialog(QWidget *parent = nullptr);
    ~BmDbDialog() override;
    bool isUpdating() const { return m_updater.isRunning(); }
    void cancelUpdate() { m_updater.cancel(); }

private slots:
    void pushButtonAddClicked();
    void pushButtonUpdateAllClicked();
    void pushButtonClearDbClicked();
    void pushButtonDeleteClicked();
    void pushButtonUpdateClicked();

signals:
    void bmDbAboutToUpdate();
    void bmDbUpdated();
    void bmDbCleared();

private:
    std::unique_ptr<Ui::BmDbDialog> ui;
    QSqlTableModel m_pathsModel;
    BmDbUpdater m_updater{this};
    DlgDbUpdate m_dbUpdateDlg{this};
    void startUpdate(const QStringList &paths);
};

#endif // BMDBDIALOG_H
