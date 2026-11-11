#include <qglobal.h>
#include "okjversion.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QProgressDialog>
#include <QSignalBlocker>
#include <QStatusBar>
#include <limits>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QMenu>
#include <QInputDialog>
#include <QFileDialog>
#include <QImageReader>
#include <QDesktopServices>
#include <QHeaderView>
#include <QTableView>
#include <QToolButton>
#include <algorithm>
#include "mzarchive.h"
#include "mediapreparation.h"
#include "dlgeditsong.h"
#include "songedit.h"
#include "soundfxbutton.h"
#include "src/models/tableviewtooltipfilter.h"
#include "dbupdater.h"
#include "okjutil.h"
#include <algorithm>
#include <memory>
#include "dlgaddsong.h"
#include <taglib.h>
#include <zip.h>
#include <gst/gst.h>
#include "okjtypes.h"

#ifdef _MSC_VER
#define NOMINMAX
#include <Windows.h>
#include <timeapi.h>
#endif

#pragma clang diagnostic push
#pragma ide diagnostic ignored "OCDFAInspection"
void MainWindow::addSfxButton(const QString &filename, const QString &label, bool reset) {
    static int numButtons = 0;
    if (reset)
        numButtons = 0;
    int col{0};
    if (numButtons % 2)
        col = 1;
    int row = numButtons / 2;
    auto button = new SoundFxButton(filename, label);
    ui->sfxButtonGrid->addWidget(button, row, col);
    connect(button, &SoundFxButton::clicked, this, &MainWindow::sfxButtonPressed);
    connect(button, &SoundFxButton::customContextMenuRequested, this,
            &MainWindow::sfxButtonContextMenuRequested);
    numButtons++;
}
#pragma clang diagnostic pop

void MainWindow::refreshSfxButtons() {
    QLayoutItem *item;
    while ((item = ui->sfxButtonGrid->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    SfxEntryList list = m_settings.getSfxEntries();
    bool first = true;
    for (const auto &entry : list) {
        if (first) {
            first = false;
            addSfxButton(entry.path, entry.name, true);
            continue;
        }
        addSfxButton(entry.path, entry.name);
    }
}

void MainWindow::updateIcons() {
    QString thm = (m_settings.theme() == 1) ? ":/theme/Icons/okjbreeze-dark/" : ":/theme/Icons/okjbreeze/";
    ui->buttonClearRotation->setIcon(QIcon(thm + "actions/22/edit-clear-all.svg"));
    ui->buttonAddSinger->setIcon(QIcon(thm + "actions/22/list-add-user.svg"));
    ui->buttonRegulars->setIcon(QIcon(thm + "actions/22/user-others.svg"));
    ui->btnRotTop->setIcon(QIcon(thm + "actions/22/go-top.svg"));
    ui->btnRotUp->setIcon(QIcon(thm + "actions/22/go-up.svg"));
    ui->btnRotBottom->setIcon(QIcon(thm + "actions/22/go-bottom.svg"));
    ui->btnRotDown->setIcon(QIcon(thm + "actions/22/go-down.svg"));
    ui->pushButton->setIcon(QIcon(thm + "actions/22/edit-find.svg"));
    ui->buttonClearSearch->setIcon(QIcon(thm + "actions/22/edit-clear.svg"));
    ui->buttonClearQueue->setIcon(QIcon(thm + "actions/22/edit-clear-all.svg"));
    ui->btnQTop->setIcon(QIcon(thm + "actions/22/go-top.svg"));
    ui->btnQUp->setIcon(QIcon(thm + "actions/22/go-up.svg"));
    ui->btnQBottom->setIcon(QIcon(thm + "actions/22/go-bottom.svg"));
    ui->btnQDown->setIcon(QIcon(thm + "actions/22/go-down.svg"));
    ui->buttonPause->setIcon(QIcon(thm + "actions/22/media-playback-pause.svg"));
    ui->buttonStop->setIcon(QIcon(thm + "actions/22/media-playback-stop.svg"));
    ui->labelVolume->setPixmap(QPixmap(thm + "actions/16/player-volume.svg"));
    ui->pushButtonTempoDn->setIcon(QIcon(thm + "actions/22/downindicator.svg"));
    ui->pushButtonTempoUp->setIcon(QIcon(thm + "actions/22/upindicator.svg"));
    ui->pushButtonKeyDn->setIcon(QIcon(thm + "actions/22/downindicator.svg"));
    ui->pushButtonKeyUp->setIcon(QIcon(thm + "actions/22/upindicator.svg"));
    ui->btnSfxStop->setIcon(QIcon(thm + "actions/22/media-playback-stop.svg"));
    ui->buttonBmPause->setIcon(QIcon(thm + "actions/22/media-playback-pause.svg"));
    ui->buttonBmStop->setIcon(QIcon(thm + "actions/22/media-playback-stop.svg"));
    ui->btnPlTop->setIcon(QIcon(thm + "actions/22/go-top.svg"));
    ui->btnPlUp->setIcon(QIcon(thm + "actions/22/go-up.svg"));
    ui->btnPlBottom->setIcon(QIcon(thm + "actions/22/go-bottom.svg"));
    ui->btnPlDown->setIcon(QIcon(thm + "actions/22/go-down.svg"));
    ui->labelVolumeBm->setPixmap(QPixmap(thm + "actions/16/player-volume.svg"));
    ui->buttonBmSearch->setIcon(QIcon(thm + "actions/22/edit-find.svg"));

    requestsDialog->updateIcons();
    treatAllSingersAsRegsChanged(m_settings.treatAllSingersAsRegs());
}

void MainWindow::setupShortcuts() {

    m_scutKSelectNextSinger.setKey(m_settings.loadShortcutKeySequence("kSelectNextSinger"));
    m_scutKSelectNextSinger.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKSelectNextSinger, &QShortcut::activated, [&]() {
        const auto nextSinger = m_rotModel.nextSinger();
        if (!nextSinger.isValid()) {
            QMessageBox::information(this, "Unable to select next",
                                     "Sorry, no unsung karaoke songs are currently in any singer's queue");
            return;
        }
        ui->tableViewRotation->clearSelection();
        ui->tableViewRotation->selectRow(nextSinger.position);
    });

    m_scutKPlayNextUnsung.setKey(m_settings.loadShortcutKeySequence("kPlayNextUnsung"));
    m_scutKPlayNextUnsung.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKPlayNextUnsung, &QShortcut::activated, this, [this] {
        const auto singer = m_rotModel.nextSinger();
        if (!singer.isValid()) {
            QMessageBox::information(this, tr("Unable to play next"),
                                     tr("Sorry, no unsung karaoke songs are currently in any singer's queue"));
            return;
        }
        requestRotationSong(singer, InterruptionWarning::PlayingOrPaused, true);
    });

    m_scutAddSinger.setKey(m_settings.loadShortcutKeySequence("addSinger"));
    m_scutAddSinger.setContext(Qt::ApplicationShortcut);
    connect(&m_scutAddSinger, &QShortcut::activated, this, &MainWindow::showAddSingerDialog);

    m_scutBFfwd.setKey(m_settings.loadShortcutKeySequence("bFfwd"));
    m_scutBFfwd.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBFfwd, &QShortcut::activated, [&]() {
        auto mediaState = m_mediaBackendBm.state();
        if (mediaState == MediaBackend::PlayingState || mediaState == MediaBackend::PausedState) {
            auto curPos = m_mediaBackendBm.position();
            auto duration = m_mediaBackendBm.duration();
            if (curPos + 5000 < duration)
                m_mediaBackendBm.setPosition(curPos + 5000);
        }
    });

    m_scutBPause.setKey(m_settings.loadShortcutKeySequence("bPause"));
    m_scutBPause.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBPause, &QShortcut::activated, [&]() {
        auto mediaState = m_mediaBackendBm.state();
        if (mediaState == MediaBackend::PlayingState) {
            m_mediaBackendBm.pause();
            ui->buttonBmPause->setChecked(true);
        } else if (mediaState == MediaBackend::PausedState) {
            m_mediaBackendBm.play(m_mediaBackendKar.state() == MediaBackend::PlayingState);
            ui->buttonBmPause->setChecked(false);
        }
    });

    m_scutBRestartSong.setKey(m_settings.loadShortcutKeySequence("bRestartSong"));
    m_scutBRestartSong.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBRestartSong, &QShortcut::activated, [&]() {
        auto mediaState = m_mediaBackendBm.state();
        if (mediaState == MediaBackend::PlayingState || mediaState == MediaBackend::PausedState) {
            m_mediaBackendBm.setPosition(0);
        }
    });
    m_scutBRwnd.setKey(m_settings.loadShortcutKeySequence("bRwnd"));
    m_scutBRwnd.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBRwnd, &QShortcut::activated, [&]() {
        auto mediaState = m_mediaBackendBm.state();
        if (mediaState == MediaBackend::PlayingState || mediaState == MediaBackend::PausedState) {
            auto curPos = m_mediaBackendBm.position();
            if (curPos - 5000 > 0)
                m_mediaBackendBm.setPosition(curPos - 5000);
            else
                m_mediaBackendBm.setPosition(0);
        }
    });

    m_scutBStop.setKey(m_settings.loadShortcutKeySequence("bStop"));
    m_scutBStop.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBStop, &QShortcut::activated, [&]() {
        m_mediaBackendBm.stop();
    });

    m_scutBVolDn.setKey(m_settings.loadShortcutKeySequence("bVolDn"));
    m_scutBVolDn.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBVolDn, &QShortcut::activated, [&]() {
        if (int curVol = m_mediaBackendBm.getVolume(); curVol > 0)
            m_mediaBackendBm.setVolume(curVol - 1);
    });

    m_scutBVolMute.setKey(m_settings.loadShortcutKeySequence("bVolMute"));
    m_scutBVolMute.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBVolMute, &QShortcut::activated, [&]() {
        m_mediaBackendBm.setMuted(!m_mediaBackendBm.isMuted());
    });

    m_scutBVolUp.setKey(m_settings.loadShortcutKeySequence("bVolUp"));
    m_scutBVolUp.setContext(Qt::ApplicationShortcut);
    connect(&m_scutBVolUp, &QShortcut::activated, [&]() {
        if (int curVol = m_mediaBackendBm.getVolume(); curVol < 100)
            m_mediaBackendBm.setVolume(curVol + 1);
    });

    m_scutJumpToSearch.setKey(m_settings.loadShortcutKeySequence("jumpToSearch"));
    m_scutJumpToSearch.setContext(Qt::ApplicationShortcut);
    connect(&m_scutJumpToSearch, &QShortcut::activated, [&]() {
        activateWindow();
        if (!hasFocus() && !ui->lineEdit->hasFocus())
            setFocus();
        if (ui->lineEdit->hasFocus())
            ui->lineEdit->clear();
        else
            ui->lineEdit->setFocus();
    });

    m_scutKFfwd.setKey(m_settings.loadShortcutKeySequence("kFfwd"));
    m_scutKFfwd.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKFfwd, &QShortcut::activated, [&]() {
        if (auto state = m_mediaBackendKar.state(); state == MediaBackend::PlayingState ||
                                                    state == MediaBackend::PausedState) {
            if (auto curPos = m_mediaBackendKar.position(); curPos + 5000 < m_mediaBackendKar.duration())
                m_mediaBackendKar.setPosition(curPos + 5000);
        }
    });

    m_scutKPause.setKey(m_settings.loadShortcutKeySequence("kPause"));
    m_scutKPause.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKPause, &QShortcut::activated, this, &MainWindow::buttonPauseClicked);

    m_scutKRestartSong.setKey(m_settings.loadShortcutKeySequence("kRestartSong"));
    m_scutKRestartSong.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKRestartSong, &QShortcut::activated, [&]() {
        if (auto state = m_mediaBackendKar.state(); state == MediaBackend::PlayingState ||
                                                    state == MediaBackend::PausedState)
            m_mediaBackendKar.setPosition(0);
    });

    m_scutKRwnd.setKey(m_settings.loadShortcutKeySequence("kRwnd"));
    m_scutKRwnd.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKRwnd, &QShortcut::activated, [&]() {
        if (auto state = m_mediaBackendKar.state(); state == MediaBackend::PlayingState ||
                                                    state == MediaBackend::PausedState) {

            if (auto curPos = m_mediaBackendKar.position(); curPos - 5000 > 0)
                m_mediaBackendKar.setPosition(curPos - 5000);
            else
                m_mediaBackendKar.setPosition(0);
        }
    });

    m_scutKStop.setKey(m_settings.loadShortcutKeySequence("kStop"));
    m_scutKStop.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKStop, &QShortcut::activated, this, &MainWindow::stopKaraokePlayback);

    m_scutKVolDn.setKey(m_settings.loadShortcutKeySequence("kVolDn"));
    m_scutKVolDn.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKVolDn, &QShortcut::activated, [&]() {
        int curVol = m_mediaBackendKar.getVolume();
        if (curVol > 0)
            m_mediaBackendKar.setVolume(curVol - 1);
    });

    m_scutKVolMute.setKey(m_settings.loadShortcutKeySequence("kVolMute"));
    m_scutKVolMute.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKVolMute, &QShortcut::activated, [&]() {
        m_mediaBackendKar.setMuted(!m_mediaBackendKar.isMuted());
    });

    m_scutKVolUp.setKey(m_settings.loadShortcutKeySequence("kVolUp"));
    m_scutKVolUp.setContext(Qt::ApplicationShortcut);
    connect(&m_scutKVolUp, &QShortcut::activated, [&]() {
        if (int curVol = m_mediaBackendKar.getVolume(); curVol < 100)
            m_mediaBackendKar.setVolume(curVol + 1);
    });

    m_scutLoadRegularSinger.setKey(m_settings.loadShortcutKeySequence("loadRegularSinger"));
    m_scutLoadRegularSinger.setContext(Qt::ApplicationShortcut);
    connect(&m_scutLoadRegularSinger, &QShortcut::activated, &m_dlgRegularSingers,
            &DlgRegularSingers::toggleVisibility);

    m_scutToggleSingerWindow.setKey(m_settings.loadShortcutKeySequence("toggleSingerWindow"));
    m_scutToggleSingerWindow.setContext(Qt::ApplicationShortcut);
    connect(&m_scutToggleSingerWindow, &QShortcut::activated, [&]() {
        cdgWindow->setVisible(!cdgWindow->isVisible());
    });

    m_scutRequests.setKey(m_settings.loadShortcutKeySequence("showIncomingRequests"));
    m_scutRequests.setContext(Qt::ApplicationShortcut);
    connect(&m_scutRequests, &QShortcut::activated, requestsDialog.get(), &DlgRequests::show);

    m_scutDeleteSinger.setParent(ui->tableViewRotation);
    m_scutDeleteSinger.setKey(QKeySequence::Delete);
    m_scutDeleteSinger.setContext(Qt::WidgetShortcut);
    connect(&m_scutDeleteSinger, &QShortcut::activated, [&]() {
        auto indexes = ui->tableViewRotation->selectionModel()->selectedRows(0);
        std::vector<int> singerIds;
        std::for_each(indexes.begin(), indexes.end(), [&](auto index) {
            singerIds.emplace_back(index.data(Qt::UserRole).toInt());
        });
        if (singerIds.empty())
            return;
        if (m_settings.showSingerRemovalWarning()) {
            QMessageBox msgBox(this);
            auto *cb = new QCheckBox("Show this warning in the future");
            cb->setChecked(m_settings.showSingerRemovalWarning());
            msgBox.setIcon(QMessageBox::Warning);
            if (singerIds.size() == 1) {
                msgBox.setText("Are you sure you want to remove this singer?");
                msgBox.setInformativeText(
                        "Unless this singer is a tracked regular, you will be unable retrieve any queue data for this singer once they are deleted.");
            } else {
                msgBox.setText("Are you sure you want to remove these singers?");
                msgBox.setInformativeText(
                        "Unless these singers are tracked regulars, you will be unable retrieve any queue data for them once they are deleted.");
            }
            QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.setCheckBox(cb);
            connect(cb, &QCheckBox::toggled, &m_settings, &Settings::setShowSingerRemovalWarning);
            msgBox.exec();
            if (msgBox.clickedButton() != yesButton) {
                return;
            }
        }

        m_qModel.loadSinger(-1);
        std::for_each(singerIds.begin(), singerIds.end(), [&](auto singerId) {
            if (m_rotModel.currentSinger() == singerId) {
                m_rotModel.setCurrentSinger(-1);
                m_rotDelegate.setCurrentSinger(-1);
            }
            m_rotModel.singerDelete(singerId);
        });
        ui->tableViewRotation->clearSelection();
        ui->tableViewQueue->clearSelection();
    });

    m_scutDeleteSong.setParent(ui->tableViewQueue);
    m_scutDeleteSong.setKey(QKeySequence::Delete);
    m_scutDeleteSong.setContext(Qt::WidgetShortcut);
    connect(&m_scutDeleteSong, &QShortcut::activated, [&]() {
        auto indexes = ui->tableViewQueue->selectionModel()->selectedRows(0);
        bool containsUnplayed{false};
        std::vector<int> songIds;
        std::for_each(indexes.begin(), indexes.end(), [&](auto index) {
            songIds.emplace_back(index.data().toInt());
            if (!m_qModel.getPlayed(index.data().toInt()))
                containsUnplayed = true;
        });
        if ((m_settings.showQueueRemovalWarning()) && containsUnplayed) {
            QMessageBox msgBox(this);
            auto *cb = new QCheckBox("Show this warning in the future");
            cb->setChecked(m_settings.showQueueRemovalWarning());
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText("Removing un-played song from queue");
            msgBox.setInformativeText("This song has not been played yet, are you sure you want to remove it?");
            QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.setCheckBox(cb);
            connect(cb, &QCheckBox::toggled, &m_settings, &Settings::setShowQueueRemovalWarning);
            msgBox.exec();
            if (msgBox.clickedButton() != yesButton)
                return;
        }
        std::for_each(songIds.begin(), songIds.end(), [&](auto songId) {
            m_qModel.remove(songId);
        });
    });

    m_scutDeletePlSong.setParent(ui->tableViewBmPlaylist);
    m_scutDeleteSong.setKey(QKeySequence::Delete);
    m_scutDeleteSong.setContext(Qt::WidgetShortcut);
    connect(&m_scutDeletePlSong, &QShortcut::activated, [&]() {
        auto rows = ui->tableViewBmPlaylist->selectionModel()->selectedRows(0);
        std::vector<int> positions;
        bool curPlayingSelected{false};
        std::for_each(rows.begin(), rows.end(), [&](auto index) {
            positions.emplace_back(index.row());
            if (m_tableModelPlaylistSongs.isCurrentlyPlayingSong(index.data(Qt::UserRole).toInt()))
                curPlayingSelected = true;
        });
        auto state = m_mediaBackendBm.state();
        if (curPlayingSelected && (state == MediaBackend::PlayingState || state == MediaBackend::PausedState)) {
            QMessageBox msgBox;
            msgBox.setWindowTitle("Unable to remove");
            msgBox.setText("The playlist song you are trying to remove is currently playing and can not be removed.");
            msgBox.exec();
            return;
        }
        std::sort(positions.begin(), positions.end());
        std::reverse(positions.begin(), positions.end());
        std::for_each(positions.begin(), positions.end(), [&](auto position) {
            m_tableModelPlaylistSongs.deleteSong(position);
        });
        if (curPlayingSelected) {
            m_tableModelPlaylistSongs.setCurrentPosition(-1);
        }
        m_tableModelPlaylistSongs.savePlaylistChanges();
        if (state != MediaBackend::PlayingState && state != MediaBackend::PausedState)
            return;
        if (ui->checkBoxBmBreak->isChecked())
            ui->labelBmNext->setText("None - Breaking after current song");
        else {
            auto nextSong = m_tableModelPlaylistSongs.getNextPlSong();
            if (nextSong.has_value())
                ui->labelBmNext->setText(nextSong->get().artist + " - " + nextSong->get().title);
            else
                ui->labelBmNext->setText("None - Breaking after current song");
        }
    });

}

void MainWindow::shortcutsUpdated() {
    m_scutKSelectNextSinger.setKey(m_settings.loadShortcutKeySequence("kSelectNextSinger"));
    m_scutKPlayNextUnsung.setKey(m_settings.loadShortcutKeySequence("kPlayNextUnsung"));
    m_scutAddSinger.setKey(m_settings.loadShortcutKeySequence("addSinger"));
    m_scutBFfwd.setKey(m_settings.loadShortcutKeySequence("bFfwd"));
    m_scutBPause.setKey(m_settings.loadShortcutKeySequence("bPause"));
    m_scutBRestartSong.setKey(m_settings.loadShortcutKeySequence("bRestartSong"));
    m_scutBRwnd.setKey(m_settings.loadShortcutKeySequence("bRwnd"));
    m_scutBStop.setKey(m_settings.loadShortcutKeySequence("bStop"));
    m_scutBVolDn.setKey(m_settings.loadShortcutKeySequence("bVolDn"));
    m_scutBVolMute.setKey(m_settings.loadShortcutKeySequence("bVolMute"));
    m_scutBVolUp.setKey(m_settings.loadShortcutKeySequence("bVolUp"));
    m_scutJumpToSearch.setKey(m_settings.loadShortcutKeySequence("jumpToSearch"));
    m_scutKFfwd.setKey(m_settings.loadShortcutKeySequence("kFfwd"));
    m_scutKPause.setKey(m_settings.loadShortcutKeySequence("kPause"));
    m_scutKRestartSong.setKey(m_settings.loadShortcutKeySequence("kRestartSong"));
    m_scutKRwnd.setKey(m_settings.loadShortcutKeySequence("kRwnd"));
    m_scutKStop.setKey(m_settings.loadShortcutKeySequence("kStop"));
    m_scutKVolDn.setKey(m_settings.loadShortcutKeySequence("kVolDn"));
    m_scutKVolMute.setKey(m_settings.loadShortcutKeySequence("kVolMute"));
    m_scutKVolUp.setKey(m_settings.loadShortcutKeySequence("kVolUp"));
    m_scutLoadRegularSinger.setKey(m_settings.loadShortcutKeySequence("loadRegularSinger"));
    m_scutRequests.setKey(m_settings.loadShortcutKeySequence("showIncomingRequests"));
    m_scutToggleSingerWindow.setKey(m_settings.loadShortcutKeySequence("toggleSingerWindow"));
}

void MainWindow::treatAllSingersAsRegsChanged(bool enabled) {
    if (enabled) {
        ui->tableViewRotation->hideColumn(TableModelRotation::COL_REGULAR);
        if (ui->tabWidgetQueue->count() == 1)
            ui->tabWidgetQueue->addTab(m_historyTabWidget, "History");
    } else {
        ui->tableViewRotation->showColumn(TableModelRotation::COL_REGULAR);
        int curSelSingerId{-1};
        if (ui->tableViewRotation->selectionModel()->selectedRows().count() > 1) {
            curSelSingerId = ui->tableViewRotation->selectionModel()->selectedRows(0).at(
                    TableModelRotation::COL_ID).data(Qt::UserRole).toInt();
        }
        if (!m_rotModel.getSinger(curSelSingerId).regular && ui->tabWidgetQueue->count() == 2)
            ui->tabWidgetQueue->removeTab(1);
    }
    autosizeRotationCols();
}

MainWindow::MainWindow(const QSqlDatabase &database, QWidget *parent) :
        QMainWindow(parent),
        ui(new Ui::MainWindow),
        m_database(database),
        rng(std::mt19937_64(std::chrono::system_clock::now().time_since_epoch().count())){
    m_logger = spdlog::get("logger");
#ifdef _MSC_VER
    timeBeginPeriod(1);
#endif
    ui->setupUi(this);
    setMouseTracking(true);
    m_songShop = std::make_unique<SongShop>(this);
    m_lazyDurationUpdater = std::make_unique<LazyDurationUpdateController>(this);
    connectDurationScanStatus();
    m_timerLazyDurationStart.setSingleShot(true);
    m_timerLazyDurationStart.setInterval(50);
    connect(&m_timerLazyDurationStart, &QTimer::timeout, this, &MainWindow::restartLazyDurationScan);
    // Connect before starting the asynchronous initial library load.
    connect(&m_karaokeSongsModel, &TableModelKaraokeSongs::libraryLoadStarted, this, [this] {
        m_libraryLoadFailed = false;
        updateDatabaseStatus();
        ui->labelLibraryLoading->setText(tr("Loading song library…"));
        ui->labelLibraryLoading->show();
        m_timerLazyDurationStart.stop();
        m_lazyDurationUpdater->stopWork();
    });
    connect(&m_karaokeSongsModel, &TableModelKaraokeSongs::libraryLoadFinished, this, [this](bool success) {
        m_libraryLoadFailed = !success;
        updateDatabaseStatus();
        ui->labelLibraryLoading->setVisible(!success || m_karaokeSongsModel.isLoading());
        if (!success)
            ui->labelLibraryLoading->setText(tr("Could not load song library. See the log for details."));
        if (success) {
            const int rows = m_historySongsModel.rowCount({});
            if (rows > 0)
                emit m_historySongsModel.dataChanged(m_historySongsModel.index(0, 0),
                    m_historySongsModel.index(rows - 1, m_historySongsModel.columnCount({}) - 1),
                    {Qt::ForegroundRole, Qt::ToolTipRole});
            restartLazyDurationScan();
        }
    });
    ui->tableViewBmPlaylist->setMouseTracking(true);
    m_historyTabWidget = ui->tabWidgetQueue->widget(1);
    ui->actionShow_Debug_Log->setChecked(m_settings.logShow());
#ifdef Q_OS_WIN
    ui->sliderBmPosition->setMaximumHeight(12);
    ui->sliderBmVolume->setMaximumWidth(12);
    ui->sliderProgress->setMaximumHeight(12);
#endif
    initDatabaseWorker();
    ui->videoPreviewBm->hide();
    ui->pushButtonKeyDn->setEnabled(false);
    ui->pushButtonKeyUp->setEnabled(false);
    ui->pushButtonTempoDn->setEnabled(false);
    ui->pushButtonTempoUp->setEnabled(false);
    m_karaokeSongsModel.loadData();
    m_rotModel.loadData();
    ui->comboBoxHistoryDblClick->addItems(QStringList{"Adds to queue", "Plays song"});
    ui->tabWidgetQueue->setCurrentIndex(0);
    ui->tableViewHistory->setModel(&m_historySongsModel);
    ui->tableViewHistory->hideColumn(0);
    ui->tableViewHistory->hideColumn(1);
    ui->tableViewHistory->hideColumn(2);
    ui->tableViewHistory->sortByColumn(TableModelHistorySongs::TITLE, Qt::AscendingOrder);
    ui->comboBoxSearchType->addItems({QString("All"), QString("Artist"), QString("Title")});
    ui->tableViewDB->hideColumn(TableModelKaraokeSongs::COL_ID);
    ui->tableViewDB->hideColumn(TableModelKaraokeSongs::COL_FILENAME);
    ui->tableViewRotation->setModel(&m_rotModel);
    ui->tableViewRotation->setItemDelegate(&m_rotDelegate);
    ui->tableViewRotation->hideColumn(TableModelRotation::COL_ADDTS);
    ui->tableViewRotation->hideColumn(TableModelRotation::COL_POSITION);
    if (m_settings.treatAllSingersAsRegs())
        ui->tableViewRotation->hideColumn(TableModelRotation::COL_REGULAR);
    if (m_settings.rotationShowNextSong()) {
        ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_NAME,
                                                                        QHeaderView::Interactive);
        ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_NEXT_SONG,
                                                                        QHeaderView::Stretch);
    } else {
        ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_NAME,
                                                                        QHeaderView::Stretch);
        ui->tableViewRotation->hideColumn(TableModelRotation::COL_NEXT_SONG);
    }
    ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_ID,
                                                                    QHeaderView::ResizeToContents);
    ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_DELETE,
                                                                    QHeaderView::ResizeToContents);
    ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_REGULAR,
                                                                    QHeaderView::ResizeToContents);
    ui->tableViewQueue->setModel(&m_qModel);
    ui->tableViewQueue->setItemDelegate(&m_qDelegate);
    ui->tableViewQueue->viewport()->installEventFilter(new TableViewToolTipFilter(ui->tableViewQueue));
    ui->labelNoSinger->setVisible(true);
    ui->tabWidgetQueue->setVisible(false);
    m_mediaTempDir = std::make_unique<QTemporaryDir>();
    dbDialog = std::make_unique<DlgDatabase>(m_karaokeSongsModel, this);
    dlgKeyChange = std::make_unique<DlgKeyChange>(&m_qModel, this);
    requestsDialog = std::make_unique<DlgRequests>(m_rotModel, m_songbookApi);
    requestsDialog->setModal(false);
    dlgSongShop = std::make_unique<DlgSongShop>(m_songShop);
    dlgSongShop->setModal(false);
    ui->tableViewDB->setModel(&m_karaokeSongsModel);
    ui->tableViewDB->sortByColumn(TableModelKaraokeSongs::COL_SONGID, Qt::AscendingOrder);
    ui->tableViewDB->viewport()->installEventFilter(new TableViewToolTipFilter(ui->tableViewDB));
    if (!MediaBackend::canPitchShift()) {
        ui->spinBoxKey->hide();
        ui->lblKey->hide();
        ui->tableViewQueue->hideColumn(7);
    }
    if (!MediaBackend::canChangeTempo()) {
        ui->spinBoxTempo->hide();
        ui->lblTempo->hide();
    }
    ui->videoPreview->setFillOnPaint(true);
    cdgWindow = std::make_unique<DlgCdg>(m_mediaBackendKar, m_mediaBackendBm, nullptr, Qt::Window);
    m_rotationDisplay = std::make_unique<RotationDisplay>(m_rotModel);
    ui->tableViewDB->hideColumn(TableModelKaraokeSongs::COL_ID);
    ui->tableViewDB->hideColumn(TableModelKaraokeSongs::COL_FILENAME);
    ui->tableViewQueue->hideColumn(TableModelQueueSongs::COL_ID);
    ui->tableViewQueue->hideColumn(TableModelQueueSongs::COL_DBSONGID);
    if (!MediaBackend::canPitchShift()) {
        ui->tableViewQueue->hideColumn(TableModelQueueSongs::COL_KEY);
    }
    m_rotModel.setHeaderData(0, Qt::Horizontal, "");
    m_rotModel.setHeaderData(1, Qt::Horizontal, "Singer");
    m_rotModel.setHeaderData(3, Qt::Horizontal, "");
    m_rotModel.setHeaderData(4, Qt::Horizontal, "");
    m_logger->info("{} Adding singer count to status bar", m_loggingPrefix);
    ui->statusBar->setContentsMargins(12, 0, 12, 0);
    ui->statusBar->addWidget(&m_labelSingerCount);
    ui->statusBar->addWidget(&m_labelRotationDuration);
    ui->statusBar->addWidget(&m_labelAutoplayStatus);
    ui->statusBar->addPermanentWidget(&m_labelDatabaseStatus);
    m_tableModelPlaylists = std::make_unique<QSqlTableModel>(this, m_database);
    m_tableModelPlaylists->setTable("bmplaylists");
    m_tableModelPlaylists->sort(2, Qt::AscendingOrder);
    bmDbDialog = std::make_unique<BmDbDialog>(this);
    ui->comboBoxBmPlaylists->setModel(m_tableModelPlaylists.get());
    ui->comboBoxBmPlaylists->setModelColumn(1);
    if (m_tableModelPlaylists->rowCount() == 0) {
        bmAddPlaylist("Default");
        ui->comboBoxBmPlaylists->setCurrentIndex(0);
    }
    ui->tableViewBmDb->setModel(&m_tableModelBreakSongs);
    m_tableModelBreakSongs.loadDatabase();
    ui->tableViewBmDb->viewport()->installEventFilter(new TableViewToolTipFilter(ui->tableViewBmDb));
    ui->tableViewBmPlaylist->setModel(&m_tableModelPlaylistSongs);
    ui->tableViewBmPlaylist->viewport()->installEventFilter(new TableViewToolTipFilter(ui->tableViewBmPlaylist));
    ui->tableViewBmPlaylist->setItemDelegate(&m_itemDelegatePlSongs);
    ui->tableViewBmDb->setColumnHidden(TableModelBreakSongs::COL_ID, true);
    ui->tableViewBmPlaylist->setColumnHidden(TableModelPlaylistSongs::COL_POSITION, true);
    m_updateChecker = std::make_unique<UpdateChecker>(this);
    m_timerButtonFlash.start(1000);
    m_logger->info("{} Initial UI setup complete", m_loggingPrefix);
    appFontChanged(m_settings.applicationFont());
    QTimer::singleShot(500, this, [this]() {
        autosizeViews();
        autosizeBmViews();
        // Keep the final startup sizing pass from overwriting host-saved widths.
        m_settings.restoreColumnWidths(ui->tableViewDB);
        m_settings.restoreColumnWidths(ui->tableViewQueue);
        m_settings.restoreColumnWidths(ui->tableViewHistory);
    });
    m_dlgRegularSingers.regularsChanged();
    m_dlgRegularSingers.setModal(false);
    updateRotationDuration();
    // The duration scan starts after the asynchronous library load finishes.
    ui->labelVolume->setPixmap(QIcon::fromTheme("player-volume").pixmap(QSize(22, 22)));
    ui->labelVolumeBm->setPixmap(QIcon::fromTheme("player-volume").pixmap(QSize(22, 22)));
    updateIcons();

    // Keep the extra view registered even when hidden, so enabling it during
    // a pause shows the retained frame without changing the playback sink.
    std::vector<QWidget *> videoWidgets{cdgWindow->getVideoDisplay(), ui->videoPreview,
                                       m_rotationDisplay->videoDisplay()};
    m_mediaBackendBm.setVideoOutputWidgets({cdgWindow->getVideoDisplayBm(), ui->videoPreviewBm});
    m_mediaBackendKar.setVideoOutputWidgets(videoWidgets);
    audioRecorder.setMusicPad(m_mediaBackendKar.recordingMusicPad());
    m_settings.setStartupOk(true);
    m_mediaBackendBm.stop(true);

    loadSettings();
    setupShortcuts();
    setupConnections();
    updateAutoplayStatus();
    updateDatabaseStatus();
    m_timerSlowUiUpdate.start(10000);
}

void MainWindow::loadSettings() {
    if (m_settings.theme() != 0) {
        ui->pushButtonIncomingRequests->setStyleSheet("");
        update();
    }
    ui->sliderBmVolume->setValue(m_settings.bmVolume());
    m_mediaBackendBm.setVolume(m_settings.bmVolume());
    ui->sliderVolume->setValue(m_settings.audioVolume());
    m_mediaBackendKar.setVolume(m_settings.audioVolume());
    m_logger->debug("{} Initial volumes - K: {} - BM {}", m_loggingPrefix, m_settings.audioVolume(), m_settings.bmVolume());
    ui->comboBoxHistoryDblClick->setCurrentIndex(m_settings.historyDblClickAction());
    if (m_settings.rotationShowNextSong())
        m_settings.restoreColumnWidths(ui->tableViewRotation);
    m_settings.restoreWindowState(cdgWindow.get());
    m_mediaBackendKar.setUseFader(m_settings.audioUseFader());
    m_mediaBackendKar.setUseSilenceDetection(m_settings.audioDetectSilence());
    m_mediaBackendBm.setUseFader(m_settings.audioUseFaderBm());
    m_mediaBackendBm.setUseSilenceDetection(m_settings.audioDetectSilenceBm());
    m_mediaBackendKar.setDownmix(m_settings.audioDownmix());
    m_mediaBackendBm.setDownmix(m_settings.audioDownmixBm());
    m_settings.restoreWindowState(requestsDialog.get());
    m_settings.restoreWindowState(dbDialog.get());
    m_settings.restoreSplitterState(ui->splitter);
    m_settings.restoreSplitterState(ui->splitter_2);
    m_settings.restoreSplitterState(ui->splitterBm);
    m_settings.restoreWindowState(dlgSongShop.get());
    m_bmCurrentPlaylist = m_settings.bmPlaylistIndex();
    ui->comboBoxBmPlaylists->setCurrentIndex(m_settings.bmPlaylistIndex());
    ui->actionDisplay_Filenames->setChecked(m_settings.bmShowFilenames());
    ui->actionDisplay_Metadata->setChecked(m_settings.bmShowMetadata());
    actionDisplayFilenamesToggled(m_settings.bmShowFilenames());
    actionDisplayMetadataToggled(m_settings.bmShowMetadata());
    m_settings.restoreSplitterState(ui->splitterBm);
    m_settings.restoreSplitterState(ui->splitter_3);
    if (m_settings.mplxMode() == Multiplex_Normal)
        pushButtonMplxBothToggled(true);
    else if (m_settings.mplxMode() == Multiplex_LeftChannel)
        pushButtonMplxLeftToggled(true);
    else if (m_settings.mplxMode() == Multiplex_RightChannel)
        pushButtonMplxRightToggled(true);
    ui->actionAutoplay_mode->setChecked(m_settings.karaokeAutoAdvance());
    m_mediaBackendKar.setEnforceAspectRatio(m_settings.enforceAspectRatio());
    m_mediaBackendBm.setEnforceAspectRatio(m_settings.enforceAspectRatio());
    m_mediaBackendKar.setEqBypass(m_settings.eqKBypass());
    m_mediaBackendBm.setEqBypass(m_settings.eqBBypass());
    for (int band = 0; band < 10; band++) {
        m_mediaBackendKar.setEqLevel(band, m_settings.getEqKLevel(band));
        m_mediaBackendBm.setEqLevel(band, m_settings.getEqBLevel(band));
    }
    ui->pushButtonIncomingRequests->setVisible(m_settings.requestServerEnabled());
    ui->btnToggleCdgWindow->setChecked(m_settings.showCdgWindow());
    ui->groupBoxNowPlaying->setVisible(m_settings.showMainWindowNowPlaying());
    ui->groupBoxSoundClips->setVisible(m_settings.showMainWindowSoundClips());
    if (m_settings.showMainWindowSoundClips()) {
        ui->groupBoxSoundClips->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->scrollAreaSoundClips->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->scrollAreaWidgetContents->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->verticalSpacerRtPanel->changeSize(0, 20, QSizePolicy::Ignored, QSizePolicy::Ignored);
        ui->groupBoxSoundClips->setVisible(true);
    } else {
        ui->groupBoxSoundClips->setVisible(false);
        ui->groupBoxSoundClips->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        ui->verticalSpacerRtPanel->changeSize(0, 20, QSizePolicy::Ignored, QSizePolicy::Expanding);
    }
    SfxEntryList list = m_settings.getSfxEntries();
    foreach (SfxEntry entry, list) {
        addSfxButton(entry.path, entry.name);
    }
    m_rotModel.setCurrentSinger(m_settings.currentRotationPosition());
    m_rotDelegate.setCurrentSinger(m_settings.currentRotationPosition());
    ui->videoPreview->setVisible(m_settings.showMainWindowVideo());
    ui->actionNow_Playing->setChecked(m_settings.showMainWindowNowPlaying());
    ui->actionSound_Clips->setChecked(m_settings.showMainWindowSoundClips());
    ui->actionVideo_Output_2->setChecked(m_settings.showMainWindowVideo());
    ui->actionMultiplex_Controls->setChecked(m_settings.showMplxControls());
    ui->widgetMplxControls->setVisible(m_settings.showMplxControls());
    ui->menuTesting->menuAction()->setVisible(m_settings.testingEnabled());
    switch (m_settings.mainWindowVideoSize()) {
        case Settings::Small:
            actionVideoSmallTriggered();
            break;
        case Settings::Medium:
            actionVideoMediumTriggered();
            break;
        case Settings::Large:
            actionVideoLargeTriggered();
            break;
    }
    comboBoxBmPlaylistsIndexChanged(m_settings.bmPlaylistIndex());
    QTimer::singleShot(250, this, [this]() {
        m_settings.restoreWindowState(this);
        m_initialUiSetupDone = true;
        rotationDataChanged();
    });
    if (m_settings.bmAutoStart()) {
        QTimer::singleShot(1000, this, [this]() {
            if (m_tableModelPlaylistSongs.rowCount() > 0)
            {
                m_tableModelPlaylistSongs.setCurrentPosition(0);
                auto plSong = m_tableModelPlaylistSongs.getCurrentSong();
                if (plSong.has_value()) {
                    if (QFile::exists(plSong->get().path)) {
                        m_mediaBackendBm.setMedia(plSong->get().path);
                        m_mediaBackendBm.play(m_mediaBackendKar.state() == MediaBackend::PlayingState);
                        m_mediaBackendBm.setVolume(ui->sliderBmVolume->value());
                    } else {
                        QMessageBox::warning(this, tr("Break music autostart failure"),
                                             tr("Break music is set to autostart but the first song in the current playlist was not found.\n\nAborting playback."),
                                             QMessageBox::Ok);
                    }
                }
            }
        });
    }
    // Without this, QAction items aren't getting set properly on program startup.  Not sure why, but it must be
    // something to do with it needing to be set after the window is finished building.
    QTimer::singleShot(250, this, [this] () {
        appFontChanged(m_settings.applicationFont());
        m_settings.restoreColumnWidths(ui->tableViewDB);
        m_settings.restoreColumnWidths(ui->tableViewQueue);
    });
}

void MainWindow::setupConnections() {
    connect(ui->btnToggleRotationWindow, &QPushButton::toggled,
            m_rotationDisplay.get(), &RotationDisplay::setDisplayVisible);
    connect(m_rotationDisplay.get(), &RotationDisplay::visibilityChanged, this, [this](bool visible) {
        const QSignalBlocker blocker(ui->btnToggleRotationWindow);
        ui->btnToggleRotationWindow->setChecked(visible);
    });
    m_rotationDisplay->setDisplayVisible(m_settings.rotationDisplayVisible());
    connect(cdgWindow.get(), &DlgCdg::visibilityChanged, ui->btnToggleCdgWindow, &QPushButton::setChecked);
    connect(ui->comboBoxHistoryDblClick, QOverload<int>::of(&QComboBox::currentIndexChanged), &m_settings,
            &Settings::setHistoryDblClickAction);
    connect(&m_rotModel, &TableModelRotation::songDroppedOnSinger, this, &MainWindow::songDroppedOnSinger);
    connect(dbDialog.get(), &DlgDatabase::databaseUpdateComplete, this, &MainWindow::databaseUpdated);
    connect(dbDialog.get(), &DlgDatabase::songChanged, this, &MainWindow::refreshLibrarySong);
    connect(dbDialog.get(), &DlgDatabase::databaseSongAdded, &m_karaokeSongsModel, &TableModelKaraokeSongs::loadData);
    connect(dbDialog.get(), &DlgDatabase::databaseSongAdded, requestsDialog.get(), &DlgRequests::databaseSongAdded);
    connect(dbDialog.get(), &DlgDatabase::databaseCleared, this, &MainWindow::databaseCleared);
    connect(&m_mediaBackendKar, &MediaBackend::volumeChanged, ui->sliderVolume, &QSlider::setValue);
    connect(&m_mediaBackendKar, &MediaBackend::positionChanged, this, &MainWindow::karaokeMediaBackend_positionChanged);
    connect(&m_mediaBackendKar, &MediaBackend::durationChanged, this, &MainWindow::karaokeMediaBackend_durationChanged);
    connect(&m_mediaBackendKar, &MediaBackend::stateChanged, this, &MainWindow::karaokeMediaBackend_stateChanged);
    connect(&m_mediaBackendKar, &MediaBackend::stopCompleted, this, [this] {
        if (m_shuttingDown || !m_pendingKaraokePlayback || m_karaokeStopReady)
            return;
        m_karaokeStopReady = true;
        QMetaObject::invokeMethod(this, &MainWindow::continueKaraokePlayback, Qt::QueuedConnection);
    });
    connect(&m_mediaBackendKar, &MediaBackend::hasActiveVideoChanged, [&](const bool &isActive) {
        m_kHasActiveVideo = isActive;
        hasActiveVideoChanged();
    });
    connect(&m_mediaBackendKar, &MediaBackend::pitchChanged, ui->spinBoxKey, &QSpinBox::setValue);
    connect(&m_mediaBackendKar, &MediaBackend::audioError, this, [this](const QString &message) {
        if (m_shuttingDown)
            return;
        const bool preparing = bool(m_mediaPreparationCancelled);
        cancelMediaPreparation();
        if (discardKaraokeStart() || preparing)
            restoreBreakMusicVolume();
        audioRecorder.stop();
        // Finish dispatching the backend error before showing a modal dialog.
        QMetaObject::invokeMethod(this, [this, message] {
            if (!m_shuttingDown)
                audioError(message);
        }, Qt::QueuedConnection);
    });
    connect(&audioRecorder, &AudioRecorder::recordingError, this, [this](const QString &message) {
        if (!m_shuttingDown)
            ui->statusBar->showMessage(tr("Recording failed: %1").arg(message), 15000);
    });
    connect(&m_mediaBackendKar, &MediaBackend::silenceDetected, this, &MainWindow::silenceDetectedKar);
    connect(&m_mediaBackendBm, &MediaBackend::audioError, this, &MainWindow::audioError);
    connect(&m_mediaBackendBm, &MediaBackend::silenceDetected, this, &MainWindow::silenceDetectedBm);
    connect(&m_mediaBackendBm, &MediaBackend::hasActiveVideoChanged, [&](const bool &isActive) {
        m_bmHasActiveVideo = isActive;
        hasActiveVideoChanged();
    });
    connect(&m_mediaBackendSfx, &MediaBackend::positionChanged, this, &MainWindow::sfxAudioBackend_positionChanged);
    connect(&m_mediaBackendSfx, &MediaBackend::durationChanged, this, &MainWindow::sfxAudioBackend_durationChanged);
    connect(&m_mediaBackendSfx, &MediaBackend::stateChanged, this, &MainWindow::sfxAudioBackend_stateChanged);
    connect(&m_rotModel, &TableModelRotation::rotationModified, this, &MainWindow::rotationDataChanged, Qt::QueuedConnection);
    connect(m_songShop.get(), &SongShop::karaokeSongDownloaded, dbDialog.get(), &DlgDatabase::singleSongAdd);
    connect(ui->pushButtonTempoDn, &QPushButton::clicked, ui->spinBoxTempo, &QSpinBox::stepDown);
    connect(ui->pushButtonTempoUp, &QPushButton::clicked, ui->spinBoxTempo, &QSpinBox::stepUp);
    connect(ui->pushButtonKeyDn, &QPushButton::clicked, ui->spinBoxKey, &QSpinBox::stepDown);
    connect(ui->pushButtonKeyUp, &QPushButton::clicked, ui->spinBoxKey, &QSpinBox::stepUp);
    connect(requestsDialog.get(), &DlgRequests::addRequestSong, &m_qModel, &TableModelQueueSongs::songAddSlot);
    connect(&m_mediaBackendBm, &MediaBackend::stateChanged, this, &MainWindow::bmMediaStateChanged);
    connect(&m_mediaBackendBm, &MediaBackend::positionChanged, this, &MainWindow::bmMediaPositionChanged);
    connect(&m_mediaBackendBm, &MediaBackend::durationChanged, this, &MainWindow::bmMediaDurationChanged);
    connect(&m_mediaBackendBm, &MediaBackend::volumeChanged, ui->sliderBmVolume, &QSlider::setValue);
    connect(bmDbDialog.get(), &BmDbDialog::bmDbUpdated, this, &MainWindow::bmDbUpdated);
    connect(bmDbDialog.get(), &BmDbDialog::bmDbCleared, this, &MainWindow::bmDbCleared);
    connect(bmDbDialog.get(), &BmDbDialog::bmDbAboutToUpdate, this, &MainWindow::bmDatabaseAboutToUpdate);
    connect(&m_timerKaraokeAA, &QTimer::timeout, this, &MainWindow::karaokeAATimerTimeout);
    connect(ui->actionAutoplay_mode, &QAction::toggled, this, [this](bool enabled) {
        m_settings.setKaraokeAutoAdvance(enabled);
        if (!enabled) {
            m_timerKaraokeAA.stop();
            updateAutoplayStatus();
            cdgWindow->showAlert(false);
        }
        updateAutoplayStatus();
    });


    connect(ui->lineEdit, &CustomLineEdit::escapePressed, ui->lineEdit, &CustomLineEdit::clear);
    connect(ui->lineEditBmSearch, &CustomLineEdit::escapePressed, ui->lineEditBmSearch, &CustomLineEdit::clear);
    connect(&m_qModel, &TableModelQueueSongs::songDroppedWithoutSinger, this, &MainWindow::songDropNoSingerSel);
    connect(ui->splitter_3, &QSplitter::splitterMoved, [&]() { autosizeViews(); });
    connect(ui->splitterBm, &QSplitter::splitterMoved, [&]() { autosizeBmViews(); });
    connect(m_updateChecker.get(), &UpdateChecker::newVersionAvailable, this, &MainWindow::newVersionAvailable);
    m_updateChecker->checkForUpdates();
    connect(&m_timerButtonFlash, &QTimer::timeout, this, &MainWindow::timerButtonFlashTimeout);
    connect(&m_timerButtonFlash, &QTimer::timeout, this, &MainWindow::updateAutoplayStatus);
    connect(&m_timerButtonFlash, &QTimer::timeout, this, &MainWindow::updateDatabaseStatus);

    connect(ui->actionSong_Shop, &QAction::triggered, [&]() { show(); });
    connect(&m_qModel, &TableModelQueueSongs::filesDroppedOnSinger, this, &MainWindow::filesDroppedOnQueue);
    connect(ui->tableViewRotation->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::tableViewRotationCurrentChanged);
    connect(&m_tableModelPlaylistSongs, &TableModelPlaylistSongs::bmSongMoved, this, &MainWindow::bmSongMoved);
    connect(&m_songbookApi, &OKJSongbookAPI::alertRecieved, this, &MainWindow::showAlert);
    connect(&m_dlgRegularSingers.historySingersModel(), &TableModelHistorySingers::historySingersModified, [&]() {
        m_historySongsModel.refresh();
    });
    connect(&m_qModel, &TableModelQueueSongs::queueModified, &m_dlgRegularSingers, &DlgRegularSingers::regularsChanged);
    connect(&m_timerSlowUiUpdate, &QTimer::timeout, this, &MainWindow::updateRotationDuration);
    connect(&m_qModel, &TableModelQueueSongs::queueModified, [&]() {
        updateRotationDuration();
        m_rotModel.layoutChanged();
    });
    connect(ui->tableViewRotation->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::rotationSelectionChanged);
    connect(ui->tableViewQueue->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::tableViewQueueSelChanged);
    connect(ui->tableViewBmPlaylist->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::tableViewPlaylistSelectionChanged);
    connect(ui->tableViewRotation->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::tableViewRotationSelChanged);
    connect(&m_tableModelPlaylistSongs, &TableModelPlaylistSongs::bmPlSongsMoved,
            [&](auto startRow, auto startCol, auto endRow, auto endCol) {
        auto topLeft = ui->tableViewBmPlaylist->model()->index(startRow, startCol);
        auto bottomRight = ui->tableViewBmPlaylist->model()->index(endRow, endCol);
        ui->tableViewBmPlaylist->clearSelection();
        ui->tableViewBmPlaylist->selectionModel()->select(QItemSelection(topLeft, bottomRight),
                                                          QItemSelectionModel::Select);
        auto nextPlSong = m_tableModelPlaylistSongs.getNextPlSong();
        if (nextPlSong.has_value() && !ui->checkBoxBmBreak->isChecked()) {
            ui->labelBmNext->setText(nextPlSong->get().artist + " - " + nextPlSong->get().title);
        } else {
            ui->labelBmNext->setText("Breaking after current song");
        }

    });
    connect(&m_tableModelPlaylistSongs, &TableModelPlaylistSongs::playingPlSongIdChanged, &m_itemDelegatePlSongs,
            &ItemDelegatePlaylistSongs::setPlayingPlSongId);
    connect(&m_qModel, &TableModelQueueSongs::qSongsMoved, [&](auto startRow, auto startCol, auto endRow, auto endCol) {
        auto topLeft = ui->tableViewQueue->model()->index(startRow, startCol);
        auto bottomRight = ui->tableViewQueue->model()->index(endRow, endCol);
        ui->tableViewQueue->clearSelection();
        ui->tableViewQueue->selectionModel()->select(QItemSelection(topLeft, bottomRight), QItemSelectionModel::Select);
    });
    connect(&m_rotModel, &TableModelRotation::singersMoved,
            [&](auto startRow, auto startCol, auto endRow, auto endCol) {
        if (startRow == endRow) {
            ui->tableViewRotation->clearSelection();
            ui->tableViewRotation->selectRow(startRow);
            return;
        }
        auto topLeft = ui->tableViewRotation->model()->index(startRow, startCol);
        auto bottomRight = ui->tableViewRotation->model()->index(endRow, endCol);
        ui->tableViewRotation->clearSelection();
        ui->tableViewRotation->selectionModel()->select(QItemSelection(topLeft, bottomRight),
                                                        QItemSelectionModel::Select);
    });
    connect(ui->buttonStop, &QPushButton::clicked, this, &MainWindow::buttonStopClicked);
    connect(ui->buttonPause, &QPushButton::clicked, this, &MainWindow::buttonPauseClicked);
    connect(ui->lineEdit, &QLineEdit::returnPressed, this, &MainWindow::search);
    connect(ui->lineEditBmSearch, &QLineEdit::returnPressed, this, &MainWindow::searchBreakMusic);
    connect(ui->tableViewDB, &QTableView::doubleClicked, this, &MainWindow::tableViewDbDoubleClicked);
    connect(ui->buttonAddSinger, &QPushButton::clicked, this, &MainWindow::showAddSingerDialog);
    connect(ui->tableViewRotation, &QTableView::doubleClicked, this, &MainWindow::tableViewRotationDoubleClicked);
    connect(ui->tableViewRotation, &QTableView::clicked, this, &MainWindow::tableViewRotationClicked);
    connect(ui->tableViewQueue, &QTableView::doubleClicked, this, &MainWindow::tableViewQueueDoubleClicked);
    connect(ui->tableViewQueue, &QTableView::clicked, this, &MainWindow::tableViewQueueClicked);
    connect(ui->tableViewBmPlaylist, &QTableView::clicked, this, &MainWindow::tableViewBmPlaylistClicked);
    connect(ui->tableViewBmPlaylist, &QTableView::doubleClicked, this, &MainWindow::tableViewBmPlaylistDoubleClicked);
    connect(ui->tableViewBmDb, &QTableView::doubleClicked, this, &MainWindow::tableViewBmDbDoubleClicked);
    connect(ui->tableViewBmDb, &QTableView::clicked, this, &MainWindow::tableViewBmDbClicked);
    connect(ui->actionManage_DB, &QAction::triggered, dbDialog.get(), &DlgDatabase::showNormal);
    connect(ui->actionManage_Karaoke_DB, &QAction::triggered, dbDialog.get(), &DlgDatabase::showNormal);
    connect(ui->actionExport_Regulars, &QAction::triggered, this, &MainWindow::actionExportRegularsTriggered);
    connect(ui->actionImport_Regulars, &QAction::triggered, this, &MainWindow::actionImportRegularsTriggered);
    connect(ui->actionSettings, &QAction::triggered, this, &MainWindow::actionSettingsTriggered);
    connect(ui->actionRegulars, &QAction::triggered, &m_dlgRegularSingers, &DlgRegularSingers::showNormal);
    connect(ui->actionIncoming_Requests, &QAction::triggered, requestsDialog.get(), &DlgRequests::show);
    connect(ui->pushButton, &QPushButton::clicked, this, &MainWindow::search);
    connect(ui->buttonClearSearch, &QPushButton::clicked, this, [this] {
        ui->lineEdit->clear();
        if (!m_settings.progressiveSearchEnabled())
            search();
        ui->lineEdit->setFocus();
    });
    connect(ui->buttonClearRotation, &QPushButton::clicked, this, &MainWindow::clearRotation);
    connect(ui->buttonClearQueue, &QPushButton::clicked, this, &MainWindow::clearSingerQueue);
    connect(ui->spinBoxKey, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::spinBoxKeyValueChanged);
    connect(ui->buttonRegulars, &QPushButton::clicked, &m_dlgRegularSingers, &DlgRegularSingers::toggleVisibility);
    connect(ui->tableViewDB, &QTableView::customContextMenuRequested, this,
            &MainWindow::tableViewDBContextMenuRequested);
    connect(ui->tableViewQueue, &QTableView::customContextMenuRequested, this,
            &MainWindow::tableViewQueueContextMenuRequested);
    connect(ui->tableViewRotation, &QTableView::customContextMenuRequested, this,
            &MainWindow::tableViewRotationContextMenuRequested);
    connect(ui->sliderProgress, &QSlider::sliderPressed, this, &MainWindow::sliderProgressPressed);
    connect(ui->sliderProgress, &QSlider::sliderReleased, this, &MainWindow::sliderProgressReleased);
    connect(ui->actionManage_Break_DB, &QAction::triggered, bmDbDialog.get(), &BmDbDialog::show);
    connect(ui->comboBoxBmPlaylists, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::comboBoxBmPlaylistsIndexChanged);
    connect(ui->checkBoxBmBreak, &QCheckBox::toggled, this, &MainWindow::checkBoxBmBreakToggled);
    connect(ui->buttonBmStop, &QPushButton::clicked, this, &MainWindow::buttonBmStopClicked);
    connect(ui->buttonBmPause, &QPushButton::clicked, this, &MainWindow::buttonBmPauseClicked);
    connect(ui->actionDisplay_Filenames, &QAction::toggled, this, &MainWindow::actionDisplayFilenamesToggled);
    connect(ui->actionDisplay_Metadata, &QAction::toggled, this, &MainWindow::actionDisplayMetadataToggled);
    connect(ui->actionPlaylistNew, &QAction::triggered, this, &MainWindow::actionPlaylistNewTriggered);
    connect(ui->actionPlaylistImport, &QAction::triggered, this, &MainWindow::actionPlaylistImportTriggered);
    connect(ui->actionPlaylistExport, &QAction::triggered, this, &MainWindow::actionPlaylistExportTriggered);
    connect(ui->actionPlaylistDelete, &QAction::triggered, this, &MainWindow::actionPlaylistDeleteTriggered);
    connect(ui->buttonBmSearch, &QPushButton::clicked, this, &MainWindow::searchBreakMusic);
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::actionAboutTriggered);
    connect(ui->pushButtonMplxBoth, &QPushButton::toggled, this, &MainWindow::pushButtonMplxBothToggled);
    connect(ui->pushButtonMplxLeft, &QPushButton::toggled, this, &MainWindow::pushButtonMplxLeftToggled);
    connect(ui->pushButtonMplxRight, &QPushButton::toggled, this, &MainWindow::pushButtonMplxRightToggled);
    connect(ui->lineEdit, &QLineEdit::textChanged, this, &MainWindow::lineEditSearchTextChanged);
    connect(ui->spinBoxTempo, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::spinBoxTempoValueChanged);
    connect(ui->actionSongbook_Generator, &QAction::triggered, [&] () {
        auto dlgBookCreator = findChild<DlgBookCreator *>(QString(), Qt::FindDirectChildrenOnly);
        if (dlgBookCreator == nullptr) {
            dlgBookCreator = new DlgBookCreator(this);
            connect(dlgBookCreator, &DlgBookCreator::finished, dlgBookCreator, &DlgBookCreator::deleteLater);
            dlgBookCreator->show();
        }
        else
            dlgBookCreator->raise();
    });
    auto *eqPanel = new DlgEq(ui->cdgGroupBox);
    eqPanel->setObjectName("equalizerPanel");
    eqPanel->setFrequencyLabelsVisible(m_settings.mainWindowVideoSize() != Settings::Small);
    ui->verticalLayout_10->insertWidget(
        ui->verticalLayout_10->indexOf(ui->widgetMplxControls), eqPanel);
    connect(eqPanel, &DlgEq::bmEqBypassChanged, &m_mediaBackendBm, &MediaBackend::setEqBypass);
    connect(eqPanel, &DlgEq::karEqBypassChanged, &m_mediaBackendKar, &MediaBackend::setEqBypass);
    connect(eqPanel, &DlgEq::bmEqLevelChanged, &m_mediaBackendBm, &MediaBackend::setEqLevel);
    connect(eqPanel, &DlgEq::karEqLevelChanged, &m_mediaBackendKar, &MediaBackend::setEqLevel);

    auto *eqButton = new QToolButton(ui->progressFrame_2);
    eqButton->setObjectName("buttonEqualizer");
    eqButton->setText(tr("EQ"));
    eqButton->setToolTip(tr("Show or hide the equalizer"));
    eqButton->setCheckable(true);
    eqButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ui->horizontalLayout_14->addWidget(eqButton);
    connect(eqButton, &QToolButton::toggled, this, [this, eqPanel, eqButton](bool expanded) {
        eqPanel->setVisible(expanded);
        eqButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_settings.setEqPanelExpanded(expanded);
    });
    eqPanel->setVisible(m_settings.eqPanelExpanded());
    eqButton->setArrowType(Qt::RightArrow);
    eqButton->setChecked(m_settings.eqPanelExpanded());
    connect(ui->pushButtonIncomingRequests, &QPushButton::clicked, requestsDialog.get(), &DlgRequests::show);
    connect(ui->pushButtonShop, &QPushButton::clicked, dlgSongShop.get(), &DlgSongShop::show);
    connect(ui->actionSong_Shop, &QAction::triggered, dlgSongShop.get(), &DlgSongShop::show);
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MainWindow::tabWidgetCurrentChanged);
    connect(ui->sliderBmPosition, &QSlider::sliderPressed, this, &MainWindow::sliderBmPositionPressed);
    connect(ui->sliderBmPosition, &QSlider::sliderReleased, this, &MainWindow::sliderBmPositionReleased);
    connect(ui->btnAddSfx, &QPushButton::clicked, this, &MainWindow::addSfxButtonPressed);
    connect(ui->btnSfxStop, &QPushButton::clicked, this, &MainWindow::stopSfxPlayback);
    connect(ui->lineEditBmSearch, &QLineEdit::textChanged, this, &MainWindow::lineEditBmSearchChanged);
    connect(ui->btnRotUp, &QPushButton::clicked, this, &MainWindow::btnRotUpClicked);
    connect(ui->btnRotTop, &QPushButton::clicked, this, &MainWindow::btnRotTopClicked);
    connect(ui->btnRotDown, &QPushButton::clicked, this, &MainWindow::btnRotDownClicked);
    connect(ui->btnRotBottom, &QPushButton::clicked, this, &MainWindow::btnRotBottomClicked);
    connect(ui->btnQUp, &QPushButton::clicked, this, &MainWindow::btnQUpClicked);
    connect(ui->btnQTop, &QPushButton::clicked, this, &MainWindow::btnQTopClicked);
    connect(ui->btnQDown, &QPushButton::clicked, this, &MainWindow::btnQDownClicked);
    connect(ui->btnQBottom, &QPushButton::clicked, this, &MainWindow::btnQBottomClicked);
    connect(ui->btnPlUp, &QPushButton::clicked, this, &MainWindow::btnPlUpClicked);
    connect(ui->btnPlTop, &QPushButton::clicked, this, &MainWindow::btnPlTopClicked);
    connect(ui->btnPlDown, &QPushButton::clicked, this, &MainWindow::btnPlDownClicked);
    connect(ui->btnPlBottom, &QPushButton::clicked, this, &MainWindow::btnPlBottomClicked);
    connect(ui->btnBmPlRandomize, &QPushButton::clicked, this, &MainWindow::btnBmPlRandomizeClicked);
    connect(ui->actionSound_Clips, &QAction::triggered, this, &MainWindow::actionSoundClipsTriggered);
    connect(ui->actionNow_Playing, &QAction::triggered, this, &MainWindow::actionNowPlayingTriggered);
    connect(ui->actionVideoLarge, &QAction::triggered, this, &MainWindow::actionVideoLargeTriggered);
    connect(ui->actionVideoSmall, &QAction::triggered, this, &MainWindow::actionVideoSmallTriggered);
    connect(ui->actionVideoMedium, &QAction::triggered, this, &MainWindow::actionVideoMediumTriggered);
    connect(ui->actionVideo_Output_2, &QAction::toggled, ui->videoPreview, &VideoDisplay::setVisible);
    connect(ui->actionVideo_Output_2, &QAction::toggled, &m_settings, &Settings::setShowMainWindowVideo);
    connect(ui->actionKaraoke_torture, &QAction::triggered, this, &MainWindow::actionKaraokeTorture);
    connect(ui->actionK_B_torture, &QAction::triggered, this, &MainWindow::actionKAndBTorture);
    connect(ui->actionBurn_in, &QAction::triggered, this, &MainWindow::actionBurnIn);
    connect(ui->actionPreview_burn_in, &QAction::triggered, this, &MainWindow::actionPreviewBurnIn);
    connect(ui->actionMultiplex_Controls, &QAction::toggled, ui->widgetMplxControls, &QWidget::setVisible);
    connect(ui->actionMultiplex_Controls, &QAction::toggled, &m_settings, &Settings::setShowMplxControls);
    connect(ui->actionCDG_Decode_Torture, &QAction::triggered, this, &MainWindow::actionCdgDecodeTorture);
    connect(ui->actionWrite_Gstreamer_pipeline_dot_files, &QAction::triggered, this,
            &MainWindow::writeGstPipelineDiagramToDisk);
    connect(ui->comboBoxSearchType, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::comboBoxSearchTypeIndexChanged);
    connect(ui->actionDocumentation, &QAction::triggered, this, &MainWindow::actionDocumentation);
    connect(ui->btnToggleCdgWindow, &QPushButton::toggled, cdgWindow.get(), &DlgCdg::setVisible);
    connect(ui->tableViewBmPlaylist, &QTableView::customContextMenuRequested, this,
            &MainWindow::tableViewBmPlaylistContextMenu);
    connect(ui->pushButtonHistoryPlay, &QPushButton::clicked, this, &MainWindow::buttonHistoryPlayClicked);
    connect(ui->pushButtonHistoryToQueue, &QPushButton::clicked, this, &MainWindow::buttonHistoryToQueueClicked);
    connect(ui->tableViewHistory, &QTableView::doubleClicked, this, &MainWindow::tableViewHistoryDoubleClicked);
    connect(ui->tableViewHistory, &QTableView::customContextMenuRequested, this,
            &MainWindow::tableViewHistoryContextMenu);
    connect(ui->actionBreak_music_torture, &QAction::triggered, this, &MainWindow::actionBreakMusicTorture);
    connect(ui->actionBurn_in_EOS_Jump, &QAction::triggered, this, &MainWindow::actionBurnInEosJump);
    // Gain changes must preserve fade envelopes, including a pending Stop.
    connect(ui->sliderVolume, &QSlider::valueChanged, &m_mediaBackendKar, &MediaBackend::setVolume);
    connect(ui->sliderBmVolume, &QSlider::valueChanged, &m_mediaBackendBm, &MediaBackend::setVolume);
    connect(ui->tabWidgetQueue, &QTabWidget::currentChanged, this, [this](int current) {
        if (current != 0 && current != 1)
            return;
        // A rapidly skipped tab has not had its layout applied yet. Do not
        // overwrite saved widths with that tab's transient geometry.
        if (!m_queueLayout.isPending()) {
            m_settings.saveColumnWidths(current == 1 ? ui->tableViewQueue : ui->tableViewHistory);
        }
        m_queueLayout.schedule();
    });

}

void MainWindow::tableViewRotationSelChanged() {
    if (ui->tableViewRotation->selectionModel()->selectedRows().empty()) {
        ui->btnRotBottom->setEnabled(false);
        ui->btnRotDown->setEnabled(false);
        ui->btnRotTop->setEnabled(false);
        ui->btnRotUp->setEnabled(false);
        ui->tabWidgetQueue->hide();
        ui->labelNoSinger->show();
    } else if (ui->tableViewRotation->selectionModel()->selectedRows().size() == 1) {
        ui->btnRotBottom->setEnabled(true);
        ui->btnRotDown->setEnabled(true);
        ui->btnRotTop->setEnabled(true);
        ui->btnRotUp->setEnabled(true);
    } else {
        ui->btnRotBottom->setEnabled(true);
        ui->btnRotDown->setEnabled(false);
        ui->btnRotTop->setEnabled(true);
        ui->btnRotUp->setEnabled(false);
    }
}

void MainWindow::tableViewPlaylistSelectionChanged() {
    if (m_playlistImporter.isRunning() || ui->tableViewBmPlaylist->selectionModel()->selectedRows().empty()) {
        ui->btnPlBottom->setEnabled(false);
        ui->btnPlDown->setEnabled(false);
        ui->btnPlTop->setEnabled(false);
        ui->btnPlUp->setEnabled(false);
    } else if (ui->tableViewBmPlaylist->selectionModel()->selectedRows().size() == 1) {
        ui->btnPlBottom->setEnabled(true);
        ui->btnPlDown->setEnabled(true);
        ui->btnPlTop->setEnabled(true);
        ui->btnPlUp->setEnabled(true);
    } else {
        ui->btnPlBottom->setEnabled(true);
        ui->btnPlDown->setEnabled(false);
        ui->btnPlTop->setEnabled(true);
        ui->btnPlUp->setEnabled(false);
    }
}

void MainWindow::tableViewQueueSelChanged() {
    if (ui->tableViewQueue->selectionModel()->selectedRows().empty()) {
        ui->btnQBottom->setEnabled(false);
        ui->btnQDown->setEnabled(false);
        ui->btnQTop->setEnabled(false);
        ui->btnQUp->setEnabled(false);
    } else if (ui->tableViewQueue->selectionModel()->selectedRows().size() == 1) {
        ui->btnQBottom->setEnabled(true);
        ui->btnQDown->setEnabled(true);
        ui->btnQTop->setEnabled(true);
        ui->btnQUp->setEnabled(true);
    } else {
        ui->btnQBottom->setEnabled(true);
        ui->btnQDown->setEnabled(false);
        ui->btnQTop->setEnabled(true);
        ui->btnQUp->setEnabled(false);
    }
}

void MainWindow::initDatabaseWorker() {
    m_dbThread = new QThread(this);
    m_dbWorker = new DatabaseWorker(m_database.databaseName());
    m_dbWorker->moveToThread(m_dbThread);

    connect(m_dbThread, &QThread::started, m_dbWorker, &DatabaseWorker::init);
    connect(m_dbThread, &QThread::finished, m_dbWorker, &QObject::deleteLater);

    // Connection: Worker reads -> Models
    connect(m_dbWorker, &DatabaseWorker::rotationLoaded, &m_rotModel, &TableModelRotation::onRotationLoaded);
    connect(m_dbWorker, &DatabaseWorker::queueLoaded, &m_qModel, &TableModelQueueSongs::onQueueLoaded);

    // Connection: Models requests -> Worker reads
    connect(&m_rotModel, &TableModelRotation::requestLoadRotation, m_dbWorker, &DatabaseWorker::loadRotation);
    connect(&m_qModel, &TableModelQueueSongs::requestLoadQueue, m_dbWorker, &DatabaseWorker::loadQueue);

    // Connection: Model writes -> Worker (Rotation)
    connect(&m_rotModel, &TableModelRotation::requestCommitRotation, m_dbWorker, &DatabaseWorker::commitRotation);
    connect(&m_rotModel, &TableModelRotation::requestAddSinger, m_dbWorker, &DatabaseWorker::addSinger);
    connect(&m_rotModel, &TableModelRotation::requestAddSingerWithSong, m_dbWorker, &DatabaseWorker::addSingerWithSong);
    connect(&m_rotModel, &TableModelRotation::requestRenameSinger, m_dbWorker, &DatabaseWorker::renameSinger);
    connect(&m_rotModel, &TableModelRotation::requestSetSingerRegular, m_dbWorker, &DatabaseWorker::setSingerRegular);
    connect(&m_rotModel, &TableModelRotation::requestClearRotation, m_dbWorker, &DatabaseWorker::clearRotation);

    // Connection: Model writes -> Worker (Queue)
    connect(&m_qModel, &TableModelQueueSongs::requestCommitQueue, m_dbWorker, &DatabaseWorker::commitQueue);
    connect(&m_qModel, &TableModelQueueSongs::requestSetQueueSongKey, m_dbWorker, &DatabaseWorker::setQueueSongKey);
    connect(&m_qModel, &TableModelQueueSongs::requestSetQueueSongPlayed, m_dbWorker, &DatabaseWorker::setQueueSongPlayed);
    connect(&m_qModel, &TableModelQueueSongs::requestRemoveAllQueueSongs, m_dbWorker, &DatabaseWorker::removeAllQueueSongs);
    connect(&m_qModel, &TableModelQueueSongs::requestAddSongToQueue, m_dbWorker, &DatabaseWorker::addSongToQueue);

    // Connection: Worker events -> Model slots
    connect(m_dbWorker, &DatabaseWorker::singerAdded, &m_rotModel, &TableModelRotation::onSingerAdded);

    connect(m_dbWorker, &DatabaseWorker::writeFailed, this, [this](int singerId) {
        m_rotModel.loadData();
        if (singerId < 0 || singerId == m_qModel.getSingerId())
            m_qModel.loadSinger(m_qModel.getSingerId());
    });
    const auto reportDatabaseError = [this](const QString &message) {
        m_logger->error("Database operation failed: {}", message.toStdString());
        QMessageBox::warning(this, tr("Database operation failed"), message);
    };
    connect(m_dbWorker, &DatabaseWorker::errorOccurred, this, reportDatabaseError);
    connect(&m_qModel, &TableModelQueueSongs::databaseWriteError, this, reportDatabaseError);

    m_dbThread->start();
}


void MainWindow::restoreBreakMusicVolume() {
    if (m_settings.bmKCrossFade())
        m_mediaBackendBm.fadeIn(false);
    else
        m_mediaBackendBm.fadeInImmediate();
}

void MainWindow::cancelMediaPreparation() {
    if (m_mediaPreparationCancelled)
        m_mediaPreparationCancelled->store(true);
    m_mediaPreparationCancelled.reset();
}

bool MainWindow::discardKaraokeStart()
{
    if (!m_startingKaraokePlayback)
        return false;
    m_startingKaraokePlayback.reset();
    m_curArtist.clear();
    m_curTitle.clear();
    m_curSinger.clear();
    ui->labelArtist->setText("None");
    ui->labelTitle->setText("None");
    ui->labelSinger->setText("None");
    return true;
}

void MainWindow::requestKaraokePlayback(KaraokePlaybackRequest request, InterruptionWarning warning)
{
    if (m_shuttingDown || request.song.path.isEmpty())
        return;

    if (m_karaokeInterruptionPrompt)
        m_karaokeInterruptionPrompt->close();
    const auto state = m_mediaBackendKar.state();
    const bool warn = !m_startingKaraokePlayback && warning != InterruptionWarning::Never &&
                      (state == MediaBackend::PlayingState ||
                       (warning == InterruptionWarning::PlayingOrPaused && state == MediaBackend::PausedState));
    if (!m_pendingKaraokePlayback && warn && m_settings.showSongInterruptionWarning()) {
        auto *dialog = new QMessageBox(QMessageBox::Warning, tr("Interrupt karaoke"),
                                       tr("Interrupt the current karaoke song?"),
                                       QMessageBox::Yes | QMessageBox::Cancel, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setDefaultButton(QMessageBox::Cancel);
        dialog->setInformativeText(tr("If you continue, the current song will be stopped."));
        auto *checkbox = new QCheckBox(tr("Show this warning in the future"));
        checkbox->setChecked(true);
        dialog->setCheckBox(checkbox);
        connect(checkbox, &QCheckBox::toggled, &m_settings, &Settings::setShowSongInterruptionWarning);
        connect(dialog, &QMessageBox::finished, this, [this, dialog, request](int answer) {
            if (m_karaokeInterruptionPrompt != dialog)
                return;
            m_karaokeInterruptionPrompt.clear();
            if (answer == QMessageBox::Yes)
                requestKaraokePlayback(request, InterruptionWarning::Never);
        });
        m_karaokeInterruptionPrompt = dialog;
        dialog->open();
        return;
    }

    cancelMediaPreparation();
    const bool wasStarting = discardKaraokeStart();
    audioRecorder.cancelPendingRecording();
    m_timerKaraokeAA.stop();
    updateAutoplayStatus();
    cdgWindow->showAlert(false);
    if (m_pendingKaraokePlayback) {
        m_pendingKaraokePlayback = std::move(request);
    } else if (!wasStarting && m_mediaBackendKar.isPlaybackStopped()) {
        startKaraokePlayback(request, request.forceCrossfade);
    } else {
        m_pendingKaraokePlayback = std::move(request);
        m_pendingKaraokeCrossfade = m_mediaBackendKar.state() == MediaBackend::PlayingState;
        // Store the request before Stop, which may complete synchronously.
        m_mediaBackendKar.stop(wasStarting);
    }
}

void MainWindow::continueKaraokePlayback()
{
    if (!m_karaokeStopReady || !m_pendingKaraokePlayback || m_shuttingDown)
        return;

    m_karaokeStopReady = false;
    auto request = std::exchange(m_pendingKaraokePlayback, std::nullopt);
    const bool crossfade = std::exchange(m_pendingKaraokeCrossfade, false) || request->forceCrossfade;
    if (m_settings.rotationAltSortOrder()) {
        const auto completed = m_rotModel.getSinger(m_rotModel.currentSinger());
        if (completed.isValid())
            m_rotModel.singerMove(completed.position, static_cast<int>(m_rotModel.singerCount() - 1));
        m_rotModel.setCurrentSinger(-1);
        m_rotDelegate.setCurrentSinger(-1);
        m_rotModel.setCurRemainSecs(0);
    }
    startKaraokePlayback(*request, crossfade);
}

void MainWindow::requestRotationSong(const okj::RotationSinger &singer, InterruptionWarning warning, bool selectSinger)
{
    const auto path = singer.nextSongPath();
    requestKaraokePlayback({
        .song = {.id = singer.nextSongQueueId(), .singerId = singer.id,
                 .dbSongId = m_karaokeSongsModel.getIdForPath(path), .keyChange = singer.nextSongKeyChg(),
                 .artist = singer.nextSongArtist(), .title = singer.nextSongTitle(),
                 .songId = singer.nextSongSongId(), .path = path},
        .selectSinger = selectSinger
    }, warning);
}

bool MainWindow::prepareKaraokeSelection(KaraokePlaybackRequest &request)
{
    if (!request.trackSong)
        return true;

    auto &song = request.song;
    const auto &singer = m_rotModel.getSinger(song.singerId);
    if (song.id > 0 && !singer.isValid())
        return false;
    if (song.id > 0) {
        // The queue could change during the fade or while the file is prepared.
        QSqlQuery query;
        query.prepare("SELECT dbsongs.path, queuesongs.keychg, dbsongs.songid FROM queuesongs "
                      "JOIN dbsongs ON dbsongs.songid = queuesongs.song "
                      "WHERE queuesongs.qsongid = ? AND queuesongs.singer = ?");
        query.addBindValue(song.id);
        query.addBindValue(song.singerId);
        if (!query.exec() || !query.next() || query.value(0).toString() != song.path)
            return false;
        song.keyChange = query.value(1).toInt();
        song.dbSongId = query.value(2).toInt();
    }

    m_curSinger = singer.name;
    m_curArtist = song.artist;
    m_curTitle = song.title;
    ui->labelSinger->setText(m_curSinger);
    ui->labelArtist->setText(m_curArtist);
    ui->labelTitle->setText(m_curTitle);
    m_mediaBackendKar.setPitchShift(song.keyChange);
    return true;
}

void MainWindow::recordKaraokeStart(const KaraokePlaybackRequest &request)
{
    if (!request.trackSong)
        return;

    const auto &song = request.song;
    const auto singer = m_rotModel.getSinger(song.singerId);
    if (song.dbSongId > 0)
        m_karaokeSongsModel.updateSongHistory(song.dbSongId);
    if (singer.isValid() && (m_settings.treatAllSingersAsRegs() || singer.regular))
        m_historySongsModel.saveSong(singer.name, song.path, song.artist, song.title, song.songId, song.keyChange);
    if (song.id > 0)
        m_qModel.setPlayedForSinger(song.singerId, song.id);
    m_rotModel.setCurrentSinger(singer.id);
    m_rotDelegate.setCurrentSinger(singer.id);
    if (singer.isValid()) {
        if (m_settings.rotationAltSortOrder() && singer.position != 0)
            m_rotModel.singerMove(singer.position, 0);
        if (request.selectSinger) {
            ui->tableViewRotation->clearSelection();
            ui->tableViewRotation->selectRow(m_rotModel.getSinger(singer.id).position);
        }
    }
}

void MainWindow::startKaraokePlayback(const KaraokePlaybackRequest &request, bool crossfade)
{
    m_kAASkip = false;
    const auto filename = request.song.path;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_mediaPreparationCancelled = cancelled;
    auto *watcher = new QFutureWatcher<PreparedKaraokeMedia>(this);
    connect(watcher, &QFutureWatcher<PreparedKaraokeMedia>::finished, this,
            [this, watcher, cancelled, selection = request, crossfade]() mutable {
        const auto prepared = watcher->result();
        watcher->deleteLater();
        if (cancelled->load() || m_shuttingDown)
            return;
        m_logger->info("{} Karaoke media preparation took {}ms", m_loggingPrefix, prepared.elapsedMs);
        if (!prepared.error.isEmpty()) {
            m_mediaPreparationCancelled.reset();
            restoreBreakMusicVolume();
            m_timerTest.stop();
            QMessageBox::warning(this, tr("Bad karaoke file"), prepared.error, QMessageBox::Ok);
            return;
        }
        if (!prepared.cdgFile.isEmpty())
            m_mediaBackendKar.setMediaCdg(prepared.cdgFile, prepared.audioFile);
        else
            m_mediaBackendKar.setMedia(prepared.audioFile);
        // Binding media can emit a synchronous error before play is requested.
        if (cancelled->load())
            return;
        m_mediaPreparationCancelled.reset();
        m_preparedPlaybackDir = prepared.directory;
        if (!prepareKaraokeSelection(selection)) {
            restoreBreakMusicVolume();
            return;
        }
        m_startingKaraokePlayback = std::move(selection);
        if (!crossfade) {
            if (m_settings.bmKCrossFade())
                m_mediaBackendBm.fadeOut(false);
            else
                m_mediaBackendBm.fadeOutImmediate();
        }
        m_mediaBackendKar.play();
        if (!m_startingKaraokePlayback && !m_karaokePlaybackActive)
            return;
        m_mediaBackendKar.fadeInImmediate();
        m_mediaBackendKar.setTempo(ui->spinBoxTempo->value());
    });
    watcher->setFuture(QtConcurrent::run([filename, cancelled] {
        if (filename.endsWith(".zip", Qt::CaseInsensitive))
            return prepareKaraokeZip(filename, cancelled);
        if (filename.endsWith(".cdg", Qt::CaseInsensitive))
            return prepareKaraokePair(filename, cancelled);
        return prepareKaraokeVideo(filename, cancelled);
    }));
}

void MainWindow::play(const QString &karaokeFilePath, const bool &k2k)
{
    requestKaraokePlayback({.song = {.path = karaokeFilePath}, .trackSong = false, .forceCrossfade = k2k},
                           InterruptionWarning::Never);
}

void MainWindow::cancelKaraokePlayback()
{
    if (m_karaokeInterruptionPrompt)
        m_karaokeInterruptionPrompt->close();
    m_pendingKaraokePlayback.reset();
    m_pendingKaraokeCrossfade = false;
    m_karaokeStopReady = false;
    cancelMediaPreparation();
    discardKaraokeStart();
}

MainWindow::~MainWindow() {
    m_shuttingDown = true;
    audioRecorder.stop();
    bmDbDialog->cancelUpdate();
    m_playlistImporter.cancel();
    cancelKaraokePlayback();
    if (m_dbThread) {
        m_dbThread->quit();
        m_dbThread->wait();
    }
    cdgWindow->stopTicker();
#ifdef _MSC_VER
    timeEndPeriod(1);
#endif
    m_lazyDurationUpdater->stopWork();
    m_settings.bmSetVolume(ui->sliderBmVolume->value());
    m_settings.setAudioVolume(ui->sliderVolume->value());
    m_logger->info("{} Saving volumes - K: {} BM {}", m_loggingPrefix, m_settings.audioVolume(), m_settings.bmVolume());
    m_logger->info("{} Saving window and widget sizing and positioning info", m_loggingPrefix);
    m_settings.saveSplitterState(ui->splitter);
    m_settings.saveSplitterState(ui->splitter_2);
    m_settings.saveSplitterState(ui->splitter_3);
    m_settings.saveColumnWidths(ui->tableViewDB);
    m_settings.saveColumnWidths(ui->tableViewRotation);
    m_settings.saveWindowState(requestsDialog.get());
    m_settings.saveWindowState(dlgSongShop.get());
    m_settings.saveWindowState(dbDialog.get());
    m_settings.saveWindowState(this);
    m_settings.saveSplitterState(ui->splitterBm);
    m_settings.saveColumnWidths(ui->tableViewBmDb);
    m_settings.saveColumnWidths(ui->tableViewBmPlaylist);
    m_settings.bmSetPlaylistIndex(ui->comboBoxBmPlaylists->currentIndex());
    m_settings.sync();
    m_logger->info("{} Program shutdown complete", m_loggingPrefix);
}

void MainWindow::search() {
    ui->tableViewDB->scrollToTop();
    m_karaokeSongsModel.search(ui->lineEdit->text());
}

void MainWindow::refreshLibrarySong(int songId) {
    const bool refreshed = m_karaokeSongsModel.refreshSong(songId);
    requestsDialog->refreshLibrarySong(songId);
    if (refreshed && m_settings.dbLazyLoadDurations() && m_karaokeSongsModel.getSong(songId).duration < 1)
        m_lazyDurationUpdater->getSongDuration(m_karaokeSongsModel.getPath(songId));
}

void MainWindow::databaseUpdated() {
    m_karaokeSongsModel.loadData();
    search();
    autosizeViews();
    m_settings.restoreColumnWidths(ui->tableViewDB);
    requestsDialog->databaseUpdateComplete();
}

void MainWindow::restartLazyDurationScan() {
    if (!m_settings.dbLazyLoadDurations())
        return;
    // The request dialog owns a second library model. Wait for both read jobs
    // before starting writes; keep this wait asynchronous as well.
    if (m_karaokeSongsModel.isLoading() || (requestsDialog && requestsDialog->isLibraryLoading())) {
        m_timerLazyDurationStart.start();
        return;
    }
    m_lazyDurationUpdater->stopWork();
    m_lazyDurationUpdater = std::make_unique<LazyDurationUpdateController>(this);
    connectDurationScanStatus();
    connect(m_lazyDurationUpdater.get(), &LazyDurationUpdateController::gotDuration, &m_karaokeSongsModel,
            &TableModelKaraokeSongs::setSongDuration);
    connect(m_lazyDurationUpdater.get(), &LazyDurationUpdateController::gotDuration, requestsDialog.get(),
            &DlgRequests::setSongDuration);
    m_lazyDurationUpdater->getDurations();
}

void MainWindow::databaseCleared() {
    m_lazyDurationUpdater->stopWork();
    m_karaokeSongsModel.loadData();
    m_rotModel.loadData();
    m_qModel.loadSinger(-1);
    ui->tableViewQueue->reset();
    autosizeViews();
    rotationDataChanged();


}

void MainWindow::buttonStopClicked() {
    if (m_mediaBackendKar.state() == MediaBackend::PlayingState) {
        if (m_settings.showSongPauseStopWarning()) {
            QMessageBox msgBox(this);
            auto *cb = new QCheckBox("Show warning on pause/stop in the future");
            cb->setChecked(m_settings.showSongPauseStopWarning());
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText("Stop currently playing karaoke song?");
            msgBox.setInformativeText(
                    "There is currently a karaoke song playing.  If you continue, the current song will be stopped.  Are you sure?");
            QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.setCheckBox(cb);
            connect(cb, &QCheckBox::toggled, &m_settings, &Settings::setShowSongPauseStopWarning);
            msgBox.exec();
            if (msgBox.clickedButton() != yesButton) {
                return;
            }
        }
    }
    stopKaraokePlayback();
}

void MainWindow::stopKaraokePlayback()
{
    const bool wasPreparing = bool(m_mediaPreparationCancelled);
    cancelKaraokePlayback();
    if (wasPreparing || !m_karaokePlaybackActive) {
        ui->labelArtist->setText("None");
        ui->labelTitle->setText("None");
        ui->labelSinger->setText("None");
    }
    m_kAASkip = true;
    m_timerKaraokeAA.stop();
    updateAutoplayStatus();
    cdgWindow->showAlert(false);
    audioRecorder.stop();
    if (m_settings.bmKCrossFade()) {
        restoreBreakMusicVolume();
        m_mediaBackendKar.stop();
    } else {
        m_mediaBackendKar.stop();
        restoreBreakMusicVolume();
    }
}

void MainWindow::buttonPauseClicked() {
    if (m_pendingKaraokePlayback || m_startingKaraokePlayback)
        return;
    if (m_mediaBackendKar.state() == MediaBackend::PausedState) {
        m_mediaBackendKar.play();
    } else if (m_mediaBackendKar.state() == MediaBackend::PlayingState) {
        if (m_settings.showSongPauseStopWarning()) {
            QMessageBox msgBox(this);
            auto *cb = new QCheckBox("Show warning on pause/stop in the future");
            cb->setChecked(m_settings.showSongPauseStopWarning());
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText("Pause currently playing karaoke song?");
            msgBox.setInformativeText(
                    "There is currently a karaoke song playing.  If you continue, the current song will be paused.  Are you sure?");
            QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.setCheckBox(cb);
            connect(cb, &QCheckBox::toggled, &m_settings, &Settings::setShowSongPauseStopWarning);
            msgBox.exec();
            if (msgBox.clickedButton() != yesButton) {
                return;
            }
        }
        m_mediaBackendKar.pause();
    }
}

void MainWindow::tableViewDbDoubleClicked(const QModelIndex &index) {
    if (!index.isValid())
        return;
    auto song = qvariant_cast<std::shared_ptr<okj::KaraokeSong>>(index.data(Qt::UserRole));
    if (m_settings.dbDoubleClickAddsSong()) {
        auto addSongDlg = new DlgAddSong(m_rotModel, m_qModel, song->id, this);
        connect(addSongDlg, &DlgAddSong::newSingerAdded, [&](auto pos) {
            ui->tableViewRotation->selectRow(pos);
            ui->lineEdit->setFocus();
        });
        addSongDlg->setModal(true);
        addSongDlg->show();
        return;
    }
    if (m_qModel.getSingerId() >= 0 && ui->tableViewRotation->selectionModel()->hasSelection()) {
        m_qModel.songAddSlot(song->id, m_qModel.getSingerId());
        updateRotationDuration();
    } else {
        QMessageBox msgBox;
        msgBox.setText("No singer selected.  You must select a singer before you can double-click to add to a queue.");
        msgBox.exec();
    }
}

void MainWindow::tableViewRotationDoubleClicked(const QModelIndex &index)
{
    if (index.isValid() && index.column() <= 3)
        requestRotationSong(m_rotModel.getSinger(index.data(Qt::UserRole).toInt()), InterruptionWarning::Playing, true);
}

void MainWindow::tableViewRotationClicked(const QModelIndex &index) {
    if (index.column() == TableModelRotation::COL_DELETE) {
        if (m_settings.showSingerRemovalWarning()) {
            QMessageBox msgBox(this);
            auto *cb = new QCheckBox("Show this warning in the future");
            cb->setChecked(m_settings.showSingerRemovalWarning());
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText("Are you sure you want to remove this singer?");
            msgBox.setInformativeText(
                    "Unless this singer is a tracked regular, you will be unable retrieve any queue data for this singer once they are deleted.");
            QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.setCheckBox(cb);
            connect(cb, &QCheckBox::toggled, &m_settings, &Settings::setShowSingerRemovalWarning);
            msgBox.exec();
            if (msgBox.clickedButton() != yesButton) {
                return;
            }
        }
        int singerId = index.data(Qt::UserRole).toInt();
        m_logger->info("{} Singer id selected: {}", m_loggingPrefix, singerId);
        m_qModel.loadSinger(-1);
        if (m_rotModel.currentSinger() == singerId) {
            m_rotModel.setCurrentSinger(-1);
            m_rotDelegate.setCurrentSinger(-1);
        }
        m_rotModel.singerDelete(singerId);
        ui->tableViewRotation->clearSelection();
        ui->tableViewQueue->clearSelection();
        return;

    }
    if (index.column() == TableModelRotation::COL_REGULAR) {
        if (!m_rotModel.getSinger(index.data(Qt::UserRole).toInt()).regular) {
            QString name = index.sibling(index.row(), TableModelRotation::COL_NAME).data().toString();
            if (m_rotModel.historySingerExists(name)) {
                auto answer = QMessageBox::question(this,
                                                    "A regular singer with this name already exists!",
                                                    "There is already a regular singer saved with this name. Would you like to load "
                                                    "the matching regular singer's history for this singer?",
                                                    QMessageBox::StandardButtons(
                                                            QMessageBox::Yes | QMessageBox::Cancel),
                                                    QMessageBox::Cancel
                );
                if (answer == QMessageBox::Yes)
                    m_rotModel.singerMakeRegular(m_rotModel.getSingerByName(name).id);
            } else
                m_rotModel.singerMakeRegular(index.data(Qt::UserRole).toInt());
        } else {
            QMessageBox msgBox(this);
            msgBox.setText("Are you sure you want to disable regular tracking for this singer?");
            msgBox.setInformativeText(
                    "Doing so will not remove the regular singer entry, but it will prevent any changes made to the singer's queue from being saved to the regular singer until the regular singer is either reloaded or the rotation singer is re-merged with the regular singer.");
            QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.exec();
            if (msgBox.clickedButton() == yesButton) {
                m_rotModel.singerDisableRegularTracking(index.data(Qt::UserRole).toInt());
            }
        }
    }
}

void MainWindow::tableViewQueueDoubleClicked(const QModelIndex &index)
{
    if (index.isValid())
        requestKaraokePlayback({.song = qvariant_cast<okj::QueueSong>(index.data(Qt::UserRole)), .selectSinger = true},
                               InterruptionWarning::Playing);
}

void MainWindow::actionExportRegularsTriggered() {
    auto exportDlg = new DlgRegularExport(m_karaokeSongsModel, this);
    exportDlg->setModal(true);
    exportDlg->show();
}

void MainWindow::actionImportRegularsTriggered() {
    auto iDialog = new DlgRegularImport(m_karaokeSongsModel, this);
    connect(iDialog, &QObject::destroyed, this, [this]() {
        m_dlgRegularSingers.regularsChanged();
        m_historySongsModel.loadSinger(m_historySongsModel.currentSingerName());
    });
    iDialog->setModal(true);
    iDialog->show();
}

void MainWindow::actionSettingsTriggered() {
    auto settingsDialog = new DlgSettings(m_mediaBackendKar, m_mediaBackendBm, m_songbookApi, this);
    settingsDialog->setModal(true);

    connect(settingsDialog, &DlgSettings::videoOffsetChanged, [&](auto offsetMs) {
        m_mediaBackendKar.setVideoOffset(offsetMs);
        m_mediaBackendBm.setVideoOffset(offsetMs);
    });
    connect(settingsDialog, &DlgSettings::requestServerEnableChanged, ui->pushButtonIncomingRequests,
            &QPushButton::setVisible);
    connect(settingsDialog, &DlgSettings::rotationShowNextSongChanged, [&]() { autosizeRotationCols(); });
    connect(settingsDialog, &DlgSettings::rotationDurationSettingsModified, this, &MainWindow::updateRotationDuration);
    connect(settingsDialog, &DlgSettings::requestServerIntervalChanged, &m_songbookApi, &OKJSongbookAPI::setInterval);
    connect(settingsDialog, &DlgSettings::shortcutsChanged, this, &MainWindow::shortcutsUpdated);

    connect(settingsDialog, &DlgSettings::treatAllSingersAsRegsChanged, this, &MainWindow::treatAllSingersAsRegsChanged);
    connect(settingsDialog, &DlgSettings::enforceAspectRatioChanged, &m_mediaBackendKar, &MediaBackend::setEnforceAspectRatio);
    connect(settingsDialog, &DlgSettings::enforceAspectRatioChanged, &m_mediaBackendBm, &MediaBackend::setEnforceAspectRatio);
    connect(settingsDialog, &DlgSettings::karaokeAutoAdvanceChanged, ui->actionAutoplay_mode, &QAction::setChecked);
    connect(settingsDialog, &DlgSettings::recordingMixLevelsChanged, &audioRecorder, &AudioRecorder::setMixLevels);
    connect(settingsDialog, &DlgSettings::audioUseFaderChanged, &m_mediaBackendKar, &MediaBackend::setUseFader);
    connect(settingsDialog, &DlgSettings::audioSilenceDetectChanged, &m_mediaBackendKar,
            &MediaBackend::setUseSilenceDetection);
    connect(settingsDialog, &DlgSettings::audioUseFaderChangedBm, &m_mediaBackendBm, &MediaBackend::setUseFader);
    connect(settingsDialog, &DlgSettings::audioSilenceDetectChangedBm, &m_mediaBackendBm,
            &MediaBackend::setUseSilenceDetection);
    connect(settingsDialog, &DlgSettings::audioDownmixChanged, &m_mediaBackendKar, &MediaBackend::setDownmix);
    connect(settingsDialog, &DlgSettings::audioDownmixChangedBm, &m_mediaBackendBm, &MediaBackend::setDownmix);
    connect(settingsDialog, &DlgSettings::applicationFontChanged, &m_itemDelegatePlSongs, &ItemDelegatePlaylistSongs::resizeIconsForFont);
    connect(settingsDialog, &DlgSettings::applicationFontChanged, &m_qDelegate, &ItemDelegateQueueSongs::resizeIconsForFont);
    connect(settingsDialog, &DlgSettings::applicationFontChanged, &m_rotDelegate, &ItemDelegateRotation::resizeIconsForFont);
    connect(settingsDialog, &DlgSettings::applicationFontChanged, &m_karaokeSongsModel, &TableModelKaraokeSongs::resizeIconsForFont);
    connect(settingsDialog, &DlgSettings::applicationFontChanged, &m_qModel, &TableModelQueueSongs::setFont);
    connect(settingsDialog, &DlgSettings::applicationFontChanged, &m_historySongsModel, &TableModelHistorySongs::setFont);
    connect(settingsDialog, &DlgSettings::applicationFontChanged, this, &MainWindow::appFontChanged);
    connect(settingsDialog, &DlgSettings::alertBgColorChanged, cdgWindow.get(), &DlgCdg::alertBgColorChanged);
    connect(settingsDialog, &DlgSettings::alertTxtColorChanged, cdgWindow.get(), &DlgCdg::alertTxtColorChanged);
    connect(settingsDialog, &DlgSettings::bgModeChanged, cdgWindow.get(), &DlgCdg::applyBackgroundImageMode);
    connect(settingsDialog, &DlgSettings::bgSlideShowDirChanged, cdgWindow.get(), &DlgCdg::applyBackgroundImageMode);
    connect(settingsDialog, &DlgSettings::cdgBgImageChanged, cdgWindow.get(), &DlgCdg::applyBackgroundImageMode);
    connect(settingsDialog, &DlgSettings::cdgOffsetsChanged, cdgWindow.get(), &DlgCdg::cdgOffsetsChanged);
    connect(settingsDialog, &DlgSettings::cdgRemainBgColorChanged, cdgWindow->durationWidget(), &TransparentWidget::setBackgroundColor);
    connect(settingsDialog, &DlgSettings::cdgRemainEnabledChanged, cdgWindow->durationWidget(), &TransparentWidget::setVisible);
    connect(settingsDialog, &DlgSettings::cdgRemainFontChanged, cdgWindow->durationWidget(), &TransparentWidget::setTextFont);
    connect(settingsDialog, &DlgSettings::cdgRemainTextColorChanged, cdgWindow->durationWidget(), &TransparentWidget::setTextColor);
    connect(settingsDialog, &DlgSettings::durationPositionReset, cdgWindow->durationWidget(), &TransparentWidget::resetPosition);
    connect(settingsDialog, &DlgSettings::karaokeAAAlertFontChanged, cdgWindow.get(), &DlgCdg::alertFontChanged);
    connect(settingsDialog, &DlgSettings::slideShowIntervalChanged, cdgWindow.get(), &DlgCdg::setSlideshowInterval);
    connect(settingsDialog, &DlgSettings::tickerBgColorChanged, cdgWindow.get(), &DlgCdg::tickerBgColorChanged);
    connect(settingsDialog, &DlgSettings::tickerEnableChanged, cdgWindow.get(), &DlgCdg::tickerEnableChanged);
    connect(settingsDialog, &DlgSettings::tickerEnableChanged, this, &MainWindow::rotationDataChanged);
    connect(settingsDialog, &DlgSettings::tickerFontChanged, cdgWindow.get(), &DlgCdg::tickerFontChanged);
    connect(settingsDialog, &DlgSettings::tickerSpeedChanged, cdgWindow.get(), &DlgCdg::tickerSpeedChanged);
    connect(settingsDialog, &DlgSettings::tickerTextColorChanged, cdgWindow.get(), &DlgCdg::tickerTextColorChanged);
    connect(settingsDialog, &DlgSettings::tickerOutputModeChanged, this, &MainWindow::rotationDataChanged);
    connect(settingsDialog, &DlgSettings::rotationDisplaySettingsChanged,
            m_rotationDisplay.get(), &RotationDisplay::applySettings);
    connect(settingsDialog, &DlgSettings::tickerCustomStringChanged, this, &MainWindow::rotationDataChanged);
    connect(settingsDialog, &DlgSettings::shortcutsChanged, this, &MainWindow::shortcutsUpdated);

    settingsDialog->show();
}

void MainWindow::songDroppedOnSinger(const int &singerId, const int &songId, const int &dropRow) {
    // Append before switching queues: loadSinger() completes asynchronously.
    m_qModel.songAddSlot(songId, singerId);
    m_qModel.loadSinger(singerId);
    ui->tableViewRotation->clearSelection();
    auto selectionModel = ui->tableViewRotation->selectionModel();
    QModelIndex topLeft;
    QModelIndex bottomRight;
    topLeft = m_rotModel.index(dropRow, 0, QModelIndex());
    bottomRight = m_rotModel.index(dropRow, 4, QModelIndex());
    QItemSelection selection(topLeft, bottomRight);
    selectionModel->select(selection, QItemSelectionModel::Select);
}

void MainWindow::tableViewQueueClicked(const QModelIndex &index) {
    if (!index.isValid() || !m_qModel.isQueueReady())
        return;
    const int songId = index.sibling(index.row(), TableModelQueueSongs::COL_ID).data().toInt();
    if (index.column() == TableModelQueueSongs::COL_PATH) {
        if ((m_settings.showQueueRemovalWarning()) &&
            (!m_qModel.getPlayed(songId))) {
            QMessageBox msgBox(this);
            auto *cb = new QCheckBox("Show this warning in the future");
            cb->setChecked(m_settings.showQueueRemovalWarning());
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText("Removing un-played song from queue");
            msgBox.setInformativeText("This song has not been played yet, are you sure you want to remove it?");
            QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
            msgBox.addButton(QMessageBox::Cancel);
            msgBox.setCheckBox(cb);
            connect(cb, &QCheckBox::toggled, &m_settings, &Settings::setShowQueueRemovalWarning);
            msgBox.exec();
            if (msgBox.clickedButton() == yesButton) {
                m_qModel.remove(songId);
            }
        } else {
            m_qModel.remove(songId);
        }
    }
}

void MainWindow::clearRotation() {
    if (m_testMode) {
        m_settings.setCurrentRotationPosition(-1);
        m_rotModel.clearRotation();
        m_rotDelegate.setCurrentSinger(-1);
        m_qModel.loadSinger(-1);
        return;
    }
    QMessageBox msgBox;
    msgBox.setText("Are you sure?");
    msgBox.setInformativeText(
            "This action will clear all rotation singers and queues. This operation can not be undone.");
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.addButton(QMessageBox::Cancel);
    QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
    msgBox.exec();
    if (msgBox.clickedButton() == yesButton) {
        m_settings.setCurrentRotationPosition(-1);
        m_rotModel.clearRotation();
        m_rotDelegate.setCurrentSinger(-1);
        m_qModel.loadSinger(-1);
    }
}

void MainWindow::clearSingerQueue() {
    if (m_testMode) {
        m_qModel.removeAll();
        return;
    }
    QMessageBox msgBox;
    msgBox.setText("Are you sure?");
    msgBox.setInformativeText(
            "This action will clear all queued songs for the selected singer.  If the singer is a regular singer, it will delete their saved regular songs as well! This operation can not be undone.");
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.addButton(QMessageBox::Cancel);
    QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
    msgBox.exec();
    if (msgBox.clickedButton() == yesButton) {
        m_qModel.removeAll();
    }
}

void MainWindow::spinBoxKeyValueChanged(const int &arg1) {
    m_mediaBackendKar.setPitchShift(arg1);
    if (arg1 > 0)
        ui->spinBoxKey->setPrefix("+");
    else
        ui->spinBoxKey->setPrefix("");
    QTimer::singleShot(20, [&]() {
        ui->spinBoxKey->findChild<QLineEdit *>()->deselect();
    });
}

void MainWindow::karaokeMediaBackend_positionChanged(const qint64 &position) {
    if (m_mediaBackendKar.state() == MediaBackend::PlayingState) {
        if (!m_sliderPositionPressed) {
            ui->sliderProgress->setMaximum((int) m_mediaBackendKar.duration());
            ui->sliderProgress->setValue((int) position);
        }
        ui->labelElapsedTime->setText(MediaBackend::msToMMSS(position));
        ui->labelRemainTime->setText(MediaBackend::msToMMSS(m_mediaBackendKar.duration() - position));
        m_rotModel.setCurRemainSecs((int) (m_mediaBackendKar.duration() - position) / 1000);
    }
}

void MainWindow::karaokeMediaBackend_durationChanged(const qint64 &duration) {
    ui->labelTotalTime->setText(MediaBackend::msToMMSS(duration));
}

void MainWindow::karaokeMediaBackend_stateChanged(const MediaBackend::State &state) {
    if (m_shuttingDown)
        return;
    if (state == MediaBackend::PlayingState && !m_karaokePlaybackActive && !m_startingKaraokePlayback)
        return;
    m_rotationDisplay->setCurrentSinger(m_curSinger, state == MediaBackend::PlayingState);
    if (state == MediaBackend::StoppedState) {
        audioRecorder.stop();
        if (discardKaraokeStart() && !m_pendingKaraokePlayback)
            restoreBreakMusicVolume();
        m_logger->info("{} MainWindow - audio backend state is now STOPPED", m_loggingPrefix);
        // Duration queries can reset the label before a terminal state arrives.
        // Process each real playback stop once; ignore startup/cleanup messages.
        if (!m_karaokePlaybackActive)
            return;
        m_karaokePlaybackActive = false;
        m_logger->info("{} KAudio entered StoppedState", m_loggingPrefix);
        ui->labelArtist->setText("None");
        ui->labelTitle->setText("None");
        ui->labelSinger->setText("None");
        ui->labelElapsedTime->setText("0:00");
        ui->labelRemainTime->setText("0:00");
        ui->labelTotalTime->setText("0:00");
        ui->sliderProgress->setValue(0);
        ui->spinBoxTempo->setValue(100);
        ui->spinBoxKey->setValue(0);
        ui->pushButtonKeyDn->setEnabled(false);
        ui->pushButtonKeyUp->setEnabled(false);
        ui->pushButtonTempoDn->setEnabled(false);
        ui->pushButtonTempoUp->setEnabled(false);
        if (m_pendingKaraokePlayback)
            return;
        restoreBreakMusicVolume();
        if (m_settings.rotationAltSortOrder()) {
            const auto completedSinger = m_rotModel.getSinger(m_rotModel.currentSinger());
            if (completedSinger.isValid())
                m_rotModel.singerMove(completedSinger.position, static_cast<int>(m_rotModel.singerCount() - 1));
            m_rotModel.setCurrentSinger(-1);
            m_rotDelegate.setCurrentSinger(-1);
            ui->tableViewRotation->clearSelection();
            if (m_rotModel.singerCount() > 0)
                ui->tableViewRotation->selectRow(0);
            m_rotModel.setCurRemainSecs(0);
        }
        if (m_settings.karaokeAutoAdvance()) {
            m_logger->info("{}  - Karaoke Autoplay is enabled", m_loggingPrefix);
            if (m_kAASkip) {
                m_kAASkip = false;
                m_logger->info("{}  - Karaoke Autoplay set to skip, bailing out", m_loggingPrefix);
            } else {
                if (!updateKaraokeAutoAdvanceTarget())
                    m_logger->info("{} KaraokeAA - No more songs to play, giving up", m_loggingPrefix);
                else {
                    const auto nextSinger = m_rotModel.getSinger(m_kAANextSinger);
                    m_logger->info("{} KaraokeAA - Will play: {} - {}", m_loggingPrefix,
                                   nextSinger.name.toStdString(), m_kAANextSongPath.toStdString());
                    m_logger->info("{} KaraokeAA - Starting {} second timer", m_loggingPrefix,
                                   m_settings.karaokeAATimeout());
                    m_timerKaraokeAA.start(m_settings.karaokeAATimeout() * 1000);
                    updateAutoplayStatus();
                    cdgWindow->setCountdownSecs(m_settings.karaokeAATimeout());
                    cdgWindow->showAlert(true);
                }
            }
        }
    }
    if (state == MediaBackend::EndOfMediaState) {
        m_logger->info("{} KAudio entered EndOfMediaState", m_loggingPrefix);
        audioRecorder.stop();
//        ipcClient->send_MessageToServer(KhIPCClient::CMD_FADE_IN);
        //m_mediaBackendBm.setVideoEnabled(true);
        m_mediaBackendKar.stop(true);
        if (!m_pendingKaraokePlayback)
            restoreBreakMusicVolume();
    }
    if (state == MediaBackend::PausedState) {
        m_logger->info("{} KAudio entered PausedState", m_loggingPrefix);
        audioRecorder.pause();
    }
    if (state == MediaBackend::PlayingState) {
        const bool resumed = std::exchange(m_karaokePlaybackActive, true);
        if (auto started = std::exchange(m_startingKaraokePlayback, std::nullopt))
            recordKaraokeStart(*started);
        if (resumed)
            audioRecorder.unpause();
        else if (!m_pendingKaraokePlayback)
            audioRecorder.record(m_curSinger, m_curArtist, m_curTitle);
        m_logger->info("{} KAudio entered PlayingState", m_loggingPrefix);
        //m_mediaBackendBm.setVideoEnabled(false);
        ui->pushButtonKeyUp->setEnabled(true);
        ui->pushButtonKeyDn->setEnabled(true);
        ui->pushButtonTempoDn->setEnabled(true);
        ui->pushButtonTempoUp->setEnabled(true);
    }
    if (state == MediaBackend::UnknownState) {
        m_logger->info("{} KAudio entered UnknownState", m_loggingPrefix);
    }
    rotationDataChanged();
}

void MainWindow::sfxAudioBackend_positionChanged(const qint64 &position) {
    ui->sliderSfxPos->setValue((int) position);
}

void MainWindow::sfxAudioBackend_durationChanged(const qint64 &duration) {
    ui->sliderSfxPos->setMaximum((int) duration);
}

void MainWindow::sfxAudioBackend_stateChanged(const MediaBackend::State &state) {
    if (state == MediaBackend::EndOfMediaState) {
        ui->sliderSfxPos->setValue(0);
        m_mediaBackendSfx.stop();
    }
    if (state == MediaBackend::StoppedState || state == MediaBackend::UnknownState)
        ui->sliderSfxPos->setValue(0);
}

void MainWindow::hasActiveVideoChanged() {
    ui->videoPreview->setHasActiveVideo(m_kHasActiveVideo);
    ui->videoPreviewBm->setHasActiveVideo(m_bmHasActiveVideo);
    cdgWindow->getVideoDisplay()->setHasActiveVideo(m_kHasActiveVideo);
    m_rotationDisplay->videoDisplay()->setHasActiveVideo(m_kHasActiveVideo);
    cdgWindow->getVideoDisplayBm()->setHasActiveVideo(m_bmHasActiveVideo);
    if (m_timerKaraokeAA.isActive() && m_settings.karaokeAAAlertEnabled())
        return;
    if (m_bmHasActiveVideo && !m_kHasActiveVideo) {
        cdgWindow->getVideoDisplay()->hide();
        cdgWindow->getVideoDisplayBm()->show();
        ui->videoPreview->hide();
        ui->videoPreviewBm->show();
    } else {
        cdgWindow->getVideoDisplay()->show();
        cdgWindow->getVideoDisplayBm()->hide();
        ui->videoPreview->show();
        ui->videoPreviewBm->hide();
    }
}

void MainWindow::rotationDataChanged() {
    if (m_shuttingDown)
        return;
    if (m_timerKaraokeAA.isActive()) {
        if (!updateKaraokeAutoAdvanceTarget()) {
            m_timerKaraokeAA.stop();
            cdgWindow->showAlert(false);
        }
        updateAutoplayStatus();
    }
    m_logger->trace("{} [{}] Called", m_loggingPrefix, __func__);
    auto st = std::chrono::high_resolution_clock::now();
    if (m_settings.rotationShowNextSong())
        autosizeRotationCols();
    updateRotationDuration();
    QString sep = "•";
    requestsDialog->rotationChanged();
    QString statusBarText = "Singers: ";
    statusBarText += QString::number(m_rotModel.singerCount());
    m_labelSingerCount.setText(statusBarText);
    QString tickerText;
    if (m_settings.tickerCustomString() != "") {
        tickerText += m_settings.tickerCustomString() + " " + sep + " ";
        QString cs = m_rotModel.getSinger(m_rotModel.currentSinger()).name;
        int nsPos;
        if (cs == "") {
            cs = m_rotModel.getSingerAtPosition(0).name;
            if (cs == "")
                cs = "[nobody]";
            nsPos = 0;
        } else
            nsPos = m_rotModel.getSinger(m_rotModel.currentSinger()).position;
        QString ns = "[nobody]";
        if (m_rotModel.singerCount() > 0) {
            if (nsPos + 1 < m_rotModel.singerCount())
                nsPos++;
            else
                nsPos = 0;
            ns = m_rotModel.getSingerAtPosition(nsPos).name;
        }
        tickerText.replace("%cs", cs);
        tickerText.replace("%ns", ns);
        tickerText.replace("%rc", QString::number(m_rotModel.singerCount()));
        if (ui->labelArtist->text() == "None" && ui->labelTitle->text() == "None")
            tickerText.replace("%curSong", "None");
        else
            tickerText.replace("%curSong", ui->labelArtist->text() + " - " + ui->labelTitle->text());
        tickerText.replace("%m_curArtist", ui->labelArtist->text());
        tickerText.replace("%m_curTitle", ui->labelTitle->text());
        tickerText.replace("%m_curSinger", cs);
        tickerText.replace("%nextSinger", ns);

    }
    if (m_settings.tickerShowRotationInfo()) {
        tickerText += "Singers: ";
        tickerText += QString::number(m_rotModel.singerCount());
        tickerText += " " + sep + " Current: ";
        int displayPos;
        QString curSingerName = m_rotModel.getSinger(m_rotModel.currentSinger()).name;
        if (m_rotModel.currentSinger() < 0)
            curSingerName = "None";
        if (curSingerName != "") {
            tickerText += curSingerName;
            displayPos = m_rotModel.getSinger(m_rotModel.currentSinger()).position;
        } else {
            tickerText += "None ";
            displayPos = -1;
        }
        int listSize;
        if (m_settings.tickerFullRotation() || (m_rotModel.singerCount() < m_settings.tickerShowNumSingers())) {
            if (curSingerName == "")
                listSize = static_cast<int>(m_rotModel.singerCount());
            else
                listSize = static_cast<int>(m_rotModel.singerCount() - 1);
            if (listSize > 0)
                tickerText += " " + sep + " Upcoming: ";
        } else {
            listSize = m_settings.tickerShowNumSingers();
            tickerText += " " + sep + " Next ";
            tickerText += QString::number(m_settings.tickerShowNumSingers());
            tickerText += " Singers: ";
        }
        for (int i = 0; i < listSize; i++) {
            if (displayPos + 1 < m_rotModel.singerCount())
                displayPos++;
            else
                displayPos = 0;
            tickerText += QString::number(i + 1);
            tickerText += ") ";
            tickerText += m_rotModel.getSingerAtPosition(displayPos).name;
            if (i < listSize - 1)
                tickerText += " ";
        }
    }
    cdgWindow->setTickerText(tickerText);

    m_logger->trace("{} [{}] finished in {}ms",
                    m_loggingPrefix,
                    __func__,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - st).count()
    );
}

void MainWindow::silenceDetectedKar() {
    m_logger->info("{} Karaoke music silence detected", m_loggingPrefix);
    m_mediaBackendKar.rawStop();
    if (!m_pendingKaraokePlayback)
        restoreBreakMusicVolume();
}

void MainWindow::silenceDetectedBm() {
    if (m_mediaBackendBm.position() > 10000 && m_mediaBackendBm.position() < (m_mediaBackendBm.duration() - 3)) {
        m_logger->info("{} Break music silence detected, reporting EndOfMediaState to trigger playlist advance",
                       m_loggingPrefix);
        bmMediaStateChanged(MediaBackend::EndOfMediaState);
    }
}

void MainWindow::tableViewDBContextMenuRequested(const QPoint &pos) {
    QModelIndex index = ui->tableViewDB->indexAt(pos);
    if (!index.isValid())
        return;
    auto song = qvariant_cast<std::shared_ptr<okj::KaraokeSong>>(index.data(Qt::UserRole));
    QMenu contextMenu(this);
    contextMenu.addAction("Preview", [&]() {
        previewKaraokeSong(song->path);
    });
    contextMenu.addSeparator();
    contextMenu.addAction("Edit", [&] () { editSong(song); });
    contextMenu.addAction("Mark bad", [&] () { markSongBad(song); });
    contextMenu.exec(QCursor::pos());
}

void MainWindow::tableViewRotationContextMenuRequested(const QPoint &pos) {
    QModelIndex index = ui->tableViewRotation->indexAt(pos);
    if (index.isValid()) {
        m_rtClickRotationSingerId = index.data(Qt::UserRole).toInt();
        QMenu contextMenu(this);
        if (ui->tableViewRotation->selectionModel()->selectedRows().size() > 1) {
            contextMenu.addAction("Delete", &m_scutDeleteSinger, &QShortcut::activated);
        } else {
            contextMenu.addAction("Rename", this, &MainWindow::renameSinger);
            contextMenu.addAction("Set as top of rotation", [&]() {
                m_rotModel.setRotationTopSingerId(m_rtClickRotationSingerId);
            });
        }
        contextMenu.exec(QCursor::pos());
    }
}

void MainWindow::sfxButtonContextMenuRequested([[maybe_unused]]const QPoint &pos) {
    auto *btn = (SoundFxButton *) sender();
    m_lastRtClickedSfxBtn.path = btn->buttonData().toString();
    m_lastRtClickedSfxBtn.name = btn->text();
    QMenu contextMenu(this);
    contextMenu.addAction("Remove", this, &MainWindow::removeSfxButton);
    contextMenu.exec(QCursor::pos());
}

void MainWindow::renameSinger() {
    bool ok;
    QString currentName = m_rotModel.getSinger(m_rtClickRotationSingerId).name;
    QString name = QInputDialog::getText(this, "Rename singer", "New name:", QLineEdit::Normal, currentName, &ok);
    if (ok && !name.isEmpty()) {
        if ((name.toLower() == currentName.toLower() && name != currentName) || !m_rotModel.singerExists(name)) {
            m_rotModel.singerSetName(m_rtClickRotationSingerId, name);
        } else if (m_rotModel.singerExists(name)) {
            QMessageBox::warning(this, "Singer exists!", "A singer named " + name +
                                                         " already exists. Please choose a unique name and try again. The operation has been cancelled.",
                                 QMessageBox::Ok);
        }

    }
}

void MainWindow::tableViewBmPlaylistContextMenu([[maybe_unused]]const QPoint &pos) {
    QMenu contextMenu(this);
    contextMenu.addAction("Delete", &m_scutDeletePlSong, &QShortcut::activated);
    contextMenu.exec(QCursor::pos());
}

void MainWindow::tableViewQueueContextMenuRequested(const QPoint &pos) {
    int selCount = ui->tableViewQueue->selectionModel()->selectedRows().size();
    if (selCount == 1) {
        QModelIndex index = ui->tableViewQueue->indexAt(pos);
        if (index.isValid()) {
            m_dbRtClickFile = index.sibling(index.row(), TableModelQueueSongs::COL_PATH).data().toString();
            m_rtClickQueueSongId = index.sibling(index.row(), 0).data().toInt();
            dlgKeyChange->setActiveSong(m_rtClickQueueSongId);
            QMenu contextMenu(this);
            contextMenu.addAction("Preview", [&] () {
                previewKaraokeSong(index.sibling(index.row(), TableModelQueueSongs::COL_PATH).data().toString());
            });
            contextMenu.addSeparator();
            contextMenu.addAction("Set Key Change", this, &MainWindow::setKeyChange);
            contextMenu.addAction("Toggle played", this, &MainWindow::toggleQueuePlayed);
            contextMenu.addSeparator();
            contextMenu.addAction("Delete", &m_scutDeleteSong, &QShortcut::activated);
            contextMenu.exec(QCursor::pos());
        }
    } else if (selCount > 1) {
        QMenu contextMenu(this);
        contextMenu.addAction("Set Played", this, &MainWindow::setMultiPlayed);
        contextMenu.addAction("Set Unplayed", this, &MainWindow::setMultiUnplayed);
        contextMenu.addSeparator();
        contextMenu.addAction("Delete", &m_scutDeleteSong, &QShortcut::activated);

        contextMenu.exec(QCursor::pos());
    }
}

void MainWindow::sliderProgressPressed() {
    m_sliderPositionPressed = true;
}

void MainWindow::sliderProgressReleased() {
    m_mediaBackendKar.setPosition(ui->sliderProgress->value());
    m_sliderPositionPressed = false;
}

void MainWindow::setKeyChange() {
    dlgKeyChange->show();
}

void MainWindow::toggleQueuePlayed() {
    m_qModel.setPlayed(m_rtClickQueueSongId, !m_qModel.getPlayed(m_rtClickQueueSongId));
    updateRotationDuration();
}

void MainWindow::previewKaraokeSong(const QString &path) {
    if (path.isEmpty())
        return;
    if (!QFile::exists(path)) {
        QMessageBox::warning(this, tr("Missing File!"),
                             "Specified karaoke file missing, preview aborted!\n\n" + path, QMessageBox::Ok);
        return;
    }
    auto *videoPreview = new DlgVideoPreview(path, this);
    if (m_testMode)
        videoPreview->setPlaybackTimeLimit(3);
    videoPreview->setAttribute(Qt::WA_DeleteOnClose);
    videoPreview->show();
}

void MainWindow::editSong(const std::shared_ptr<okj::KaraokeSong>& song) {
    const int songId = song->id;
    const QString originalPath = song->path;
    const QString artist = song->artist;
    const QString title = song->title;
    const QString discId = song->songid;
    TableModelKaraokeSourceDirs model;
    const SourceDir sourceDir = model.getDirByPath(originalPath);
    const auto pattern = sourceDir.getPattern();
    const bool showSongId = pattern != SourceDir::AT && pattern != SourceDir::TA;
    const bool allowRename = sourceDir.getIndex() != -1 &&
                             pattern != SourceDir::CUSTOM && pattern != SourceDir::METADATA;
    if (sourceDir.getIndex() == -1) {
        QMessageBox::warning(this, tr("Song folder is not configured"),
                             tr("Add the file's source folder in the database settings to rename it."));
    }

    DlgEditSong dialog(artist, title, discId, showSongId, allowRename, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    okj::SongEdit edit{dialog.artist(), dialog.title(), dialog.songId(), {}};
    if (artist == edit.artist && title == edit.title && discId == edit.discId)
        return;
    if (dialog.renameFile()) {
        switch (pattern) {
        case SourceDir::SAT: edit.filenameBase = edit.discId + " - " + edit.artist + " - " + edit.title; break;
        case SourceDir::STA: edit.filenameBase = edit.discId + " - " + edit.title + " - " + edit.artist; break;
        case SourceDir::ATS: edit.filenameBase = edit.artist + " - " + edit.title + " - " + edit.discId; break;
        case SourceDir::TAS: edit.filenameBase = edit.title + " - " + edit.artist + " - " + edit.discId; break;
        case SourceDir::S_T_A: edit.filenameBase = edit.discId + '_' + edit.title + '_' + edit.artist; break;
        case SourceDir::AT: edit.filenameBase = edit.artist + " - " + edit.title; break;
        case SourceDir::TA: edit.filenameBase = edit.title + " - " + edit.artist; break;
        case SourceDir::CUSTOM:
        case SourceDir::METADATA:
            QMessageBox::warning(this, tr("Unable to rename file"),
                                 tr("This source folder's naming pattern does not support renaming."));
            return;
        }
    }

    const auto error = okj::saveSongEdit(m_database, songId, originalPath, edit);
    if (!error.isEmpty()) {
        m_logger->error("{} Song edit failed: {}", m_loggingPrefix, error.toStdString());
        QMessageBox message(QMessageBox::Warning, tr("Song edit failed"), tr("Could not save the song edit."),
                            QMessageBox::Ok, this);
        message.setTextFormat(Qt::PlainText);
        message.setInformativeText(error);
        message.exec();
        return;
    }

    refreshLibrarySong(songId);
    if (dialog.renameFile()) {
        m_qModel.loadSinger(m_qModel.getSingerId());
        m_rotModel.loadData();
        m_historySongsModel.loadSinger(m_historySongsModel.currentSingerName());
    }
    QMessageBox::information(this, tr("Edit successful"), tr("The song edit was saved."));
}

void MainWindow::markSongBad(const std::shared_ptr<okj::KaraokeSong>& song) {
    QMessageBox msgBox;
    QMessageBox msgBoxResult;
    msgBox.setWindowTitle("Mark song as bad?");
    msgBox.setText("Would you like mark the file as bad in the DB, or remove it from disk permanently?");
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setInformativeText(song->path);
    auto markBadButton = msgBox.addButton(tr("Mark Bad"), QMessageBox::ActionRole);
    auto removeFileButton = msgBox.addButton(tr("Remove File"), QMessageBox::ActionRole);
    auto cancelButton = msgBox.addButton(QMessageBox::Cancel);
    msgBox.exec();
    if (msgBox.clickedButton() == markBadButton) {
        m_karaokeSongsModel.markSongBad(song->path);
        msgBoxResult.setText("File marked as bad and will no longer show up in searches.");
        msgBoxResult.setIcon(QMessageBox::Information);
        msgBoxResult.exec();
    } else if (msgBox.clickedButton() == removeFileButton) {
        bool isCdg = false;
        if (QFileInfo(song->path).suffix().toLower() == "cdg")
            isCdg = true;
        QString mediaFile;
        if (isCdg)
            mediaFile = findMatchingAudioFile(song->path);
        QFile file(song->path);
        auto ret = m_karaokeSongsModel.removeBadSong(song->path);
        switch (ret) {
            case TableModelKaraokeSongs::DELETE_OK:
                msgBoxResult.setText("File removed successfully");
                msgBoxResult.setIcon(QMessageBox::Information);
                msgBoxResult.exec();
                break;
            case TableModelKaraokeSongs::DELETE_FAIL:
                msgBoxResult.setText("Error while removing file");
                msgBoxResult.setIcon(QMessageBox::Warning);
                msgBoxResult.setInformativeText(
                        "Unable to remove the file.  Please check file permissions.\nOperation cancelled.");
                msgBoxResult.exec();
                break;
            case TableModelKaraokeSongs::DELETE_CDG_AUDIO_FAIL:
                msgBoxResult.setText("Error while removing file");
                msgBoxResult.setIcon(QMessageBox::Warning);
                msgBoxResult.setInformativeText(
                        "The cdg file was deleted, but there was an error while deleting the matching media file.  You will need to manually remove the file.");
                msgBoxResult.exec();
                break;
        }
    }
}

void MainWindow::karaokeAATimerTimeout() {
    m_logger->info("{} KaraokeAA - timer timeout", m_loggingPrefix);
    m_timerKaraokeAA.stop();
    updateAutoplayStatus();
    cdgWindow->showAlert(false);
    if (m_kAASkip || !m_settings.karaokeAutoAdvance()) {
        m_logger->info("{} KaraokeAA - Countdown cancelled", m_loggingPrefix);
        m_kAASkip = false;
    } else {
        requestRotationSong(m_rotModel.nextSinger(), InterruptionWarning::Never, false);
    }
}

void MainWindow::timerButtonFlashTimeout() {
    static QString normalSS = " \
        QPushButton { \
            border: 2px solid #8f8f91; \
            border-radius: 6px; \
            background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #f6f7fa, stop: 1 #dadbde); \
            min-width: 80px; \
            padding-left: 5px; \
            padding-right: 5px; \
            padding-top: 3px; \
            padding-bottom: 3px; \
        } \
        QPushButton:pressed { \
            background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #dadbde, stop: 1 #f6f7fa); \
        } \
        QPushButton:flat { \
            border: none; /* no border for a flat push button */ \
        } \
        QPushButton:default { \
            border-color: navy; /* make the default button prominent */ \
        }";

    static QString blinkSS = " \
        QPushButton { \
            border: 2px solid #8f8f91; \
            border-radius: 6px; \
            background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #f6f700, stop: 1 #dadb00); \
            min-width: 80px; \
            padding-left: 5px; \
            padding-right: 5px; \
            padding-top: 3px; \
            padding-bottom: 3px; \
        } \
        QPushButton:pressed { \
            background-color: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #dadbde, stop: 1 #f6f7fa); \
        } \
        QPushButton:flat { \
            border: none; /* no border for a flat push button */ \
        } \
        QPushButton:default { \
            border-color: navy; /* make the default button prominent */ \
        } \
    ";

    if (requestsDialog->numRequests() > 0) {
        static bool flashed = false;
        if (m_settings.theme() != 0) {
            auto normal = this->palette().button().color();
            auto blink = QColor("yellow");
            auto blinkTxt = QColor("black");
            auto normalTxt = this->palette().buttonText().color();
            auto palette = QPalette(ui->pushButtonIncomingRequests->palette());
            palette.setColor(QPalette::Button, (flashed) ? normal : blink);
            palette.setColor(QPalette::ButtonText, (flashed) ? normalTxt : blinkTxt);
            ui->pushButtonIncomingRequests->setPalette(palette);
            ui->pushButtonIncomingRequests->setText(
                    " Requests (" + QString::number(requestsDialog->numRequests()) + ") ");
            flashed = !flashed;
        } else {
            ui->pushButtonIncomingRequests->setText(
                    " Requests (" + QString::number(requestsDialog->numRequests()) + ") ");
            ui->pushButtonIncomingRequests->setStyleSheet((flashed) ? normalSS : blinkSS);
            flashed = !flashed;
        }
        update();
    } else if (ui->pushButtonIncomingRequests->text() != "Requests") {
        if (m_settings.theme() != 0) {
            ui->pushButtonIncomingRequests->setPalette(this->palette());
            ui->pushButtonIncomingRequests->setText("Requests");
        } else {
            ui->pushButtonIncomingRequests->setStyleSheet(normalSS);
            ui->pushButtonIncomingRequests->setText(" Requests ");
        }
        update();
    }
}

void MainWindow::bmAddPlaylist(const QString &title) {
    if (m_tableModelPlaylists->insertRow(m_tableModelPlaylists->rowCount())) {
        QModelIndex index = m_tableModelPlaylists->index(m_tableModelPlaylists->rowCount() - 1, 1);
        m_tableModelPlaylists->setData(index, title);
        m_tableModelPlaylists->submitAll();
        m_tableModelPlaylists->select();
        ui->comboBoxBmPlaylists->setCurrentIndex(index.row());
    }
}

void MainWindow::bmDbUpdated() {
    if (m_shuttingDown)
        return;
    m_tableModelBreakSongs.loadDatabase();
    ui->actionPlaylistImport->setEnabled(!m_playlistImporter.isRunning());
}

void MainWindow::bmDbCleared() {
    m_playlistImporter.cancel();
    m_logger->info("{} bmDbCleared fired", m_loggingPrefix);
    m_tableModelBreakSongs.loadDatabase();
    bmAddPlaylist("Default");
    ui->comboBoxBmPlaylists->setCurrentIndex(0);
}

void MainWindow::bmMediaStateChanged(const MediaBackend::State &newState) {
    static MediaBackend::State lastState = MediaBackend::StoppedState;
    if (newState == lastState)
        return;
    lastState = newState;
    switch (newState) {
        case MediaBackend::StoppedState:
            resetBmLabels();
            break;
        case MediaBackend::EndOfMediaState: {
            if (ui->checkBoxBmBreak->isChecked()) {
                ui->checkBoxBmBreak->setChecked(false);
                m_mediaBackendBm.stop(true);
                resetBmLabels();
                return;
            }
            auto plSong = m_tableModelPlaylistSongs.getNextPlSong();
            if (plSong.has_value()) {
                m_mediaBackendBm.setMedia(plSong->get().path);
                m_tableModelPlaylistSongs.setCurrentPosition(plSong->get().position);
                m_logger->info("{} Break music auto-advancing to song: {}", m_loggingPrefix,
                               plSong->get().path.toStdString());
                m_mediaBackendBm.stop(true);
                m_mediaBackendBm.play(m_mediaBackendKar.state() == MediaBackend::PlayingState);
            } else {
                m_mediaBackendBm.stop(true);
                resetBmLabels();
            }
            break;
        }
        case MediaBackend::PlayingState: {
            auto plSong = m_tableModelPlaylistSongs.getCurrentSong();
            if (plSong.has_value())
                ui->labelBmPlaying->setText(plSong->get().artist + " - " + plSong->get().title);
            auto plNextSong = m_tableModelPlaylistSongs.getNextPlSong();
            if (!ui->checkBoxBmBreak->isChecked() && plNextSong.has_value())
                ui->labelBmNext->setText(plNextSong->get().artist + " - " + plNextSong->get().title);
            else
                ui->labelBmNext->setText("None - Breaking after current song");
            break;
        }
        case MediaBackend::PausedState:
            break;
        case MediaBackend::UnknownState:
            resetBmLabels();
            break;
    }
}

void MainWindow::bmMediaPositionChanged(const qint64 &position) {
    if (!m_sliderBmPositionPressed) {
        ui->sliderBmPosition->setValue((int) position);
    }
    ui->labelBmPosition->setText(QTime(0, 0, 0, 0).addMSecs((int) position).toString("m:ss"));
    ui->labelBmRemaining->setText(
            QTime(0, 0, 0, 0).addMSecs((int) (m_mediaBackendBm.duration() - position)).toString("m:ss"));
}

void MainWindow::bmMediaDurationChanged(const qint64 &duration) {
    ui->sliderBmPosition->setMaximum((int) duration);
    ui->labelBmDuration->setText(QTime(0, 0, 0, 0).addMSecs((int) duration).toString("m:ss"));
}

void MainWindow::tableViewBmPlaylistClicked(const QModelIndex &index) {
    if (index.column() == TableModelPlaylistSongs::COL_PATH) {
        if (m_tableModelPlaylistSongs.isCurrentlyPlayingSong(index.data(Qt::UserRole).toInt())) {
            if (m_mediaBackendBm.state() == MediaBackend::PlayingState ||
                m_mediaBackendBm.state() == MediaBackend::PausedState) {
                QMessageBox msgBox;
                msgBox.setWindowTitle("Unable to remove");
                msgBox.setText(
                        "The playlist song you are trying to remove is currently playing and can not be removed.");
                msgBox.exec();
                return;
            }
            m_tableModelPlaylistSongs.setCurrentPosition(-1);
            resetBmLabels();
        }
        m_tableModelPlaylistSongs.deleteSong(index.row());
        m_tableModelPlaylistSongs.savePlaylistChanges();
        if (m_tableModelPlaylistSongs.currentPosition() > index.row()) {
            m_tableModelPlaylistSongs.setCurrentPosition(m_tableModelPlaylistSongs.currentPosition() - 1);
        }
        if (ui->checkBoxBmBreak->isChecked()) {
            ui->labelBmNext->setText("None - Breaking after current song");
            return;
        }
        auto nextSong = m_tableModelPlaylistSongs.getNextPlSong();
        if (nextSong.has_value())
            ui->labelBmNext->setText(nextSong->get().artist + " - " + nextSong->get().title);
        else
            ui->labelBmNext->setText("None - Breaking after current song");
    }
}

void MainWindow::comboBoxBmPlaylistsIndexChanged(const int &index) {
    m_bmCurrentPlaylist = m_tableModelPlaylists->index(index, 0).data().toInt();
    m_tableModelPlaylistSongs.setCurrentPlaylist(m_bmCurrentPlaylist);
    auto nextPlSong = m_tableModelPlaylistSongs.getNextPlSong();
    if (nextPlSong.has_value())
        ui->labelBmNext->setText(nextPlSong->get().artist + " - " + nextPlSong->get().title);
    else
        ui->labelBmNext->setText("None - Breaking after current song");
    ui->tableViewBmPlaylist->clearSelection();
}

void MainWindow::checkBoxBmBreakToggled(const bool &checked) {
    if (!checked) {
        auto nextSong = m_tableModelPlaylistSongs.getNextPlSong();
        if (nextSong.has_value())
            ui->labelBmNext->setText(nextSong->get().artist + " - " + nextSong->get().title);
        return;
    }
    ui->labelBmNext->setText("None - Stopping after current song");
}

void MainWindow::tableViewBmDbDoubleClicked(const QModelIndex &index) {
    int songId = index.sibling(index.row(), 0).data().toInt();
    m_tableModelPlaylistSongs.addSong(songId);
    m_tableModelPlaylistSongs.savePlaylistChanges();
}

void MainWindow::buttonBmStopClicked() {
    m_mediaBackendBm.stop(false);
}

void MainWindow::searchBreakMusic() {
    m_tableModelBreakSongs.search(ui->lineEditBmSearch->text());
}

void MainWindow::tableViewBmPlaylistDoubleClicked(const QModelIndex &index) {
    if (m_mediaBackendBm.state() == MediaBackend::PlayingState || m_mediaBackendBm.state() == MediaBackend::PausedState)
        m_mediaBackendBm.stop(false);
    m_tableModelPlaylistSongs.setCurrentPosition(index.row());
    auto plSong = m_tableModelPlaylistSongs.getCurrentSong();
    if (!plSong.has_value())
        return;
    m_mediaBackendBm.setMedia(plSong->get().path);
    m_mediaBackendBm.play(m_mediaBackendKar.state() == MediaBackend::PlayingState);
    if (m_mediaBackendKar.state() != MediaBackend::PlayingState)
        m_mediaBackendBm.fadeInImmediate();
}

void MainWindow::buttonBmPauseClicked(const bool &checked) {
    if (checked)
        m_mediaBackendBm.pause();
    else
        m_mediaBackendBm.play(m_mediaBackendKar.state() == MediaBackend::PlayingState);
}

bool MainWindow::bmPlaylistExists(const QString &name) {
    for (int i = 0; i < m_tableModelPlaylists->rowCount(); i++) {
        if (m_tableModelPlaylists->index(i, 1).data().toString().toLower() == name.toLower())
            return true;
    }
    return false;
}

void MainWindow::actionDisplayMetadataToggled(const bool &arg1) {
    ui->tableViewBmDb->setColumnHidden(TableModelBreakSongs::COL_ARTIST, !arg1);
    ui->tableViewBmDb->setColumnHidden(TableModelBreakSongs::COL_TITLE, !arg1);
    ui->tableViewBmPlaylist->setColumnHidden(TableModelPlaylistSongs::COL_ARTIST, !arg1);
    ui->tableViewBmPlaylist->setColumnHidden(TableModelPlaylistSongs::COL_TITLE, !arg1);
    m_settings.bmSetShowMetadata(arg1);
    autosizeBmViews();
}

void MainWindow::actionDisplayFilenamesToggled(const bool &arg1) {
    ui->tableViewBmDb->setColumnHidden(TableModelBreakSongs::COL_FILENAME, !arg1);
    ui->tableViewBmPlaylist->setColumnHidden(TableModelPlaylistSongs::COL_FILENAME, !arg1);
    m_settings.bmSetShowFilenames(arg1);
    autosizeBmViews();
}

void MainWindow::actionPlaylistNewTriggered() {
    bool ok;
    QString title = QInputDialog::getText(this, tr("New Playlist"), tr("Playlist title:"), QLineEdit::Normal,
                                          tr("New Playlist"), &ok);
    if (ok && !title.isEmpty()) {
        bmAddPlaylist(title);
    }
}

void MainWindow::actionPlaylistImportTriggered() {
    if (m_playlistImporter.isRunning() || bmDbDialog->isUpdating())
        return;
#ifdef Q_OS_LINUX
    QString importFile = QFileDialog::getOpenFileName(
            this,
            tr("Select playlist to import"),
            QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0),
            tr("m3u playlist(*.m3u)"),
            nullptr,
            QFileDialog::DontUseNativeDialog
    );
#else
    QString importFile = QFileDialog::getOpenFileName(
            this,
            tr("Select playlist to import"),
            QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation).at(0),
            tr("m3u playlist(*.m3u)"),
            nullptr
    );
#endif
    if (importFile.isEmpty())
        return;
    bool ok = false;
    const auto title = QInputDialog::getText(this, tr("New Playlist"), tr("Playlist title:"),
                                             QLineEdit::Normal, tr("New Playlist"), &ok);
    if (!ok || title.isEmpty() || m_playlistImporter.isRunning() || bmDbDialog->isUpdating())
        return;
    BreakPlaylistImportRequest request{m_database.databaseName(), importFile, title};
    QSqlQuery destination(m_database);
    destination.prepare("SELECT playlistid FROM bmplaylists WHERE title=:title COLLATE NOCASE");
    destination.bindValue(":title", title);
    if (!destination.exec()) {
        QMessageBox::warning(this, tr("Playlist import failed"), destination.lastError().text());
        return;
    }
    if (destination.next())
        request.playlistId = destination.value(0).toInt();
    destination.finish();
    const int selectedAtStart = m_bmCurrentPlaylist;
    auto *progress = new QProgressDialog(tr("Reading playlist…"), tr("Cancel"), 0, 0, this);
    progress->setWindowTitle(tr("Import Playlist"));
    progress->setWindowModality(Qt::NonModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    // Playlist edits rewrite the displayed list. Pause them until the worker's
    // append has been reflected in the models, so a stale view cannot erase it.
    const QList<QWidget *> widgets{ui->tableViewBmDb, ui->tableViewBmPlaylist,
        ui->btnPlTop, ui->btnPlUp, ui->btnPlDown, ui->btnPlBottom, ui->btnBmPlRandomize, bmDbDialog.get()};
    const QList<QAction *> actions{ui->actionPlaylistImport, ui->actionPlaylistNew,
        ui->actionPlaylistDelete, ui->actionManage_Break_DB};
    QList<bool> widgetEnabled, actionEnabled;
    for (auto *widget : widgets) {
        widgetEnabled.append(widget->isEnabled());
        widget->setEnabled(false);
    }
    for (auto *action : actions) {
        actionEnabled.append(action->isEnabled());
        action->setEnabled(false);
    }
    connect(progress, &QProgressDialog::canceled, &m_playlistImporter, &BreakPlaylistImporter::cancel);
    connect(&m_playlistImporter, &BreakPlaylistImporter::progressChanged, progress,
            [progress](qint64 processed, qint64 total) {
        if (total > 0) {
            const auto limit = std::numeric_limits<int>::max();
            progress->setRange(0, static_cast<int>(std::min<qint64>(total, limit)));
            progress->setValue(static_cast<int>(std::min<qint64>(processed, limit)));
            progress->setLabelText(processed == total ? tr("Saving playlist…") :
                tr("Reading song metadata… %1 of %2").arg(processed).arg(total));
        }
    });
    connect(&m_playlistImporter, &BreakPlaylistImporter::finished, progress,
            [this, progress, selectedAtStart, widgets, actions, widgetEnabled, actionEnabled]
            (const BreakPlaylistImportResult &result) {
        progress->hide();
        progress->deleteLater();
        for (qsizetype index = 0; index < widgets.size(); ++index)
            widgets[index]->setEnabled(widgetEnabled[index]);
        for (qsizetype index = 0; index < actions.size(); ++index)
            actions[index]->setEnabled(actionEnabled[index]);
        tableViewPlaylistSelectionChanged();
        if (m_shuttingDown || result.cancelled)
            return;
        if (!result.error.isEmpty()) {
            QMessageBox::warning(this, tr("Playlist import failed"), result.error);
            return;
        }
        m_tableModelBreakSongs.loadDatabase();
        // Keep a later user selection; otherwise display the import destination.
        const int desired = m_bmCurrentPlaylist == selectedAtStart ? result.playlistId : m_bmCurrentPlaylist;
        int row = -1;
        {
            const QSignalBlocker block(ui->comboBoxBmPlaylists);
            m_tableModelPlaylists->select();
            while (m_tableModelPlaylists->canFetchMore())
                m_tableModelPlaylists->fetchMore();
            for (int index = 0; index < m_tableModelPlaylists->rowCount(); ++index) {
                if (m_tableModelPlaylists->index(index, 0).data().toInt() == desired) {
                    row = index;
                    break;
                }
            }
            if (row < 0 && m_tableModelPlaylists->rowCount() > 0)
                row = 0;
            ui->comboBoxBmPlaylists->setCurrentIndex(row);
        }
        const int selected = m_tableModelPlaylists->index(row, 0).data().toInt();
        if (selected != m_bmCurrentPlaylist || selected == result.playlistId)
            comboBoxBmPlaylistsIndexChanged(row);
        statusBar()->showMessage(tr("Imported %1 songs; skipped %2 unavailable files.")
                                 .arg(result.imported).arg(result.skipped), 10000);
    });
    m_playlistImporter.start(request);
    progress->show();
}

void MainWindow::actionPlaylistExportTriggered() {
    QString defaultFilePath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QDir::separator() +
                              ui->comboBoxBmPlaylists->currentText() + ".m3u";
    m_logger->debug("{} Default save location: {}", m_loggingPrefix, defaultFilePath.toStdString());
#ifdef Q_OS_LINUX
    QString saveFilePath = QFileDialog::getSaveFileName(
            this,
            tr("Select filename to save playlist as"),
            defaultFilePath,
            tr("m3u playlist(*.m3u)"),
            nullptr,
            QFileDialog::DontUseNativeDialog
    );
#else
    QString saveFilePath = QFileDialog::getSaveFileName(
            this,
            tr("Select filename to save playlist as"),
            defaultFilePath,
            tr("m3u playlist(*.m3u)"),
            nullptr
    );
#endif
    if (saveFilePath != "") {
        QFile file(saveFilePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, tr("Error saving file"),
                                 tr("Unable to open selected file for writing.  Please verify that you have the proper permissions to write to that location."),
                                 QMessageBox::Close);
            return;
        }
        QTextStream out(&file);
        for (int i = 0; i < m_tableModelPlaylistSongs.rowCount(); i++) {
            out << m_tableModelPlaylistSongs.index(i, TableModelPlaylistSongs::COL_PATH).data().toString() << "\n";
        }
    }
}

void MainWindow::actionPlaylistDeleteTriggered() {
    QMessageBox msgBox;
    msgBox.setText("Are you sure?");
    msgBox.setInformativeText(
            "Are you sure you want to delete the current playlist?  If you have not exported it, you will not be able to undo this action!");
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.addButton(QMessageBox::Cancel);
    QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
    msgBox.exec();
    if (msgBox.clickedButton() == yesButton) {
        QSqlQuery query;
        query.exec("DELETE FROM bmplsongs WHERE playlist == " +
                   QString::number(m_tableModelPlaylistSongs.currentPlaylist()));
        query.exec(
                "DELETE FROM bmplaylists WHERE playlistid == " +
                QString::number(m_tableModelPlaylistSongs.currentPlaylist()));
        m_tableModelPlaylists->select();
        if (m_tableModelPlaylists->rowCount() == 0) {
            bmAddPlaylist("Default");
        }
        ui->comboBoxBmPlaylists->setCurrentIndex(0);
        m_tableModelPlaylistSongs.setCurrentPlaylist(m_tableModelPlaylists->index(0, 0).data().toInt());
    }
}

void MainWindow::actionAboutTriggered() {
    QString title;
    QString text;
    QString date = QString::fromLocal8Bit(__DATE__) + " " + QString(__TIME__);
    title = "About OpenKJ";
    text =  "OpenKJ\nVersion: " + QString(OKJ_VERSION_STRING) + " " + QString(OKJ_VERSION_BRANCH);
    text += "\nLicense: GNU GPL v3+";
    text += "\nBuilt: " + date;
    text += "\n\nIncluded library info";
    text += "\n\nQt version: " + QString(QT_VERSION_STR);
    text += "\nLicense: GNU GPL v3";
    text += "\n\nGStreamer version: " + QString(gst_version_string());
    text += "\nLicense: GNU LGPL v2.1";
    text += "\n\nspdlog version: " + QString::number(SPDLOG_VER_MAJOR) + '.' + QString::number(SPDLOG_VER_MINOR) + '.' + QString::number(SPDLOG_VER_PATCH);
    text += "\nLicense: MIT";
    text += "\n\nTagLib version: " + QString::number(TAGLIB_MAJOR_VERSION) + '.' + QString::number(TAGLIB_MINOR_VERSION) + '.' + QString::number(TAGLIB_PATCH_VERSION);
    text += "\nLicense: GNU LGPL v2.1";
    text += "\n\nlibzip version: " + QString(LIBZIP_VERSION);
    text += "\nLicense: BSD-3-Clause";
    QMessageBox::about(this, title, text);
}

void MainWindow::pushButtonMplxLeftToggled(const bool &checked) {
    if (!checked)
        return;
    m_settings.setMplxMode(Multiplex_LeftChannel);
    m_mediaBackendKar.setMplxMode(Multiplex_LeftChannel);

}

void MainWindow::pushButtonMplxBothToggled(const bool &checked) {
    if (!checked)
        return;
    m_settings.setMplxMode(Multiplex_Normal);
    m_mediaBackendKar.setMplxMode(Multiplex_Normal);
}

void MainWindow::pushButtonMplxRightToggled(const bool &checked) {
    if (!checked)
        return;
    m_settings.setMplxMode(Multiplex_RightChannel);
    m_mediaBackendKar.setMplxMode(Multiplex_RightChannel);
}

void MainWindow::lineEditSearchTextChanged(const QString &arg1) {
    if (!m_settings.progressiveSearchEnabled())
        return;
    ui->tableViewDB->scrollToTop();
    static QString lastVal;
    if (arg1.trimmed() != lastVal) {
        m_karaokeSongsModel.search(arg1);
        lastVal = arg1.trimmed();
    }
}

void MainWindow::setMultiPlayed() {
    QModelIndexList indexes = ui->tableViewQueue->selectionModel()->selectedIndexes();
    QModelIndex index;

            foreach(index, indexes) {
            if (index.column() == 0) {
                int queueId = index.sibling(index.row(), 0).data().toInt();
                m_logger->info("{} Selected row: {} - queueId: {}", m_loggingPrefix, index.row(), queueId);
                m_qModel.setPlayed(queueId);
            }
        }
}

void MainWindow::setMultiUnplayed() {
    QModelIndexList indexes = ui->tableViewQueue->selectionModel()->selectedIndexes();
    QModelIndex index;

            foreach(index, indexes) {
            if (index.column() == 0) {
                int queueId = index.sibling(index.row(), 0).data().toInt();
                m_logger->info("{} Selected row: {} - queueId: {}", m_loggingPrefix, index.row(), queueId);
                m_qModel.setPlayed(queueId, false);
            }
        }
}

void MainWindow::spinBoxTempoValueChanged(const int &arg1) {
    m_mediaBackendKar.setTempo(arg1);
    QTimer::singleShot(20, [&]() {
        ui->spinBoxTempo->findChild<QLineEdit *>()->deselect();
    });
}

void MainWindow::audioError(const QString &msg) {
    QMessageBox msgBox;
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setText("Audio playback error! - " + msg);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.exec();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_mediaBackendKar.state() == MediaBackend::PlayingState) {
        QMessageBox msgBox(this);
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setText("Are you sure you want to exit?");
        msgBox.setInformativeText(
                "There is currently a karaoke song playing.  If you continue, the current song will be stopped. Are you sure?");
        QPushButton *yesButton = msgBox.addButton(QMessageBox::Yes);
        msgBox.addButton(QMessageBox::Cancel);
        msgBox.exec();
        if (msgBox.clickedButton() != yesButton) {
            event->ignore();
            return;
        }
    }
    m_rotationDisplay->saveForShutdown();
    if (!m_settings.cdgWindowFullscreen())
        m_settings.saveWindowState(cdgWindow.get());
    m_settings.setShowCdgWindow(cdgWindow->isVisible());
    cdgWindow->setVisible(false);
    dlgSongShop->setVisible(false);
    requestsDialog->setVisible(false);
    bmDbDialog->cancelUpdate();
    m_playlistImporter.cancel();
    dbDialog->cancelScan();
    cancelKaraokePlayback();
    audioRecorder.stop();
    event->accept();
}

void MainWindow::songDropNoSingerSel() {
    QMessageBox msgBox;
    msgBox.setText("No singer selected.  You must select a singer before you can add songs to a queue.");
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.exec();
}

void MainWindow::newVersionAvailable(const QString &version, const QUrl &releasePage, bool preview)
{
    auto *notice = new QMessageBox(QMessageBox::Information, tr("OpenKJ update"),
                                   preview ? tr("OpenKJ %1 preview is available.").arg(version)
                                           : tr("OpenKJ %1 is available.").arg(version),
                                   QMessageBox::Close, this);
    notice->setAttribute(Qt::WA_DeleteOnClose);
    notice->setWindowModality(Qt::NonModal);
    auto *viewRelease = notice->addButton(tr("View release"), QMessageBox::ActionRole);
    connect(viewRelease, &QPushButton::clicked, notice, [releasePage] {
        QDesktopServices::openUrl(releasePage);
    });
    notice->show();
}

void MainWindow::filesDroppedOnQueue(const QList<QUrl> &urls, const int &singerId, const int &position) {
            foreach (QUrl url, urls) {
            QString file = url.toLocalFile();
            if (QFile(file).exists()) {
                if (file.endsWith(".zip", Qt::CaseInsensitive)) {
                    MzArchive archive(file);
                    if (!archive.isValidKaraokeFile()) {
                        QMessageBox msgBox;
                        msgBox.setWindowTitle("Invalid karaoke file!");
                        msgBox.setText("Invalid karaoke file dropped on queue");
                        msgBox.setIcon(QMessageBox::Warning);
                        msgBox.setInformativeText(file);
                        msgBox.exec();
                        continue;
                    }
                } else if (file.endsWith(".cdg", Qt::CaseInsensitive)) {
                    if (m_karaokeSongsModel.findCdgAudioFile(file) == QString()) {
                        QMessageBox msgBox;
                        msgBox.setWindowTitle("Invalid karaoke file!");
                        msgBox.setIcon(QMessageBox::Warning);
                        msgBox.setText("CDG file dropped on queue has no matching audio file");
                        msgBox.setInformativeText(file);
                        msgBox.exec();
                        continue;
                    }
                } else if (!file.endsWith(".mp4", Qt::CaseInsensitive) && !file.endsWith(".mkv", Qt::CaseInsensitive) &&
                           !file.endsWith(".avi", Qt::CaseInsensitive) && !file.endsWith(".m4v", Qt::CaseInsensitive)) {
                    QMessageBox msgBox;
                    msgBox.setWindowTitle("Invalid karaoke file!");
                    msgBox.setText(
                            "Unsupported file type dropped on queue.  Supported file types: mp3+g zip, cdg, mp4, mkv, avi");
                    msgBox.setIcon(QMessageBox::Warning);
                    msgBox.setInformativeText(file);
                    msgBox.exec();
                    continue;
                }
                m_logger->info("{} Karaoke file dropped. Singer: {} Pos: {} Path: {}", m_loggingPrefix, singerId,
                               position, file.toStdString());
                QFileInfo dFileInfo(file);

                okj::KaraokeSong droppedSong{
                        -1,
                        "--Dropped Song--",
                        "--dropped song--",
                        dFileInfo.completeBaseName(),
                        dFileInfo.completeBaseName().toLower(),
                        "!!DROPPED!!",
                        "!!dropped!!",
                        0,
                        dFileInfo.fileName(),
                        file,
                        "",
                        0,
                        QDateTime()
                };
                int songId = m_karaokeSongsModel.addSong(droppedSong);
                m_logger->info("{} addSong returned songid: {}", m_loggingPrefix, songId);
                if (songId == -1)
                    continue;
                if (singerId == m_qModel.getSingerId() && m_qModel.isQueueReady())
                    m_qModel.insert(songId, position);
            }
        }
}

void MainWindow::appFontChanged(const QFont &font) {
    QApplication::setFont(font);
    ui->actionAbout->setFont(font);
    auto smallerFont = font;
    smallerFont.setPointSize(font.pointSize() - 2);
    auto smallerFontBold = smallerFont;
    smallerFontBold.setBold(true);
    ui->labelTotal->setFont(smallerFont);
    ui->labelTotalTime->setFont(smallerFont);
    ui->labelElapsed->setFont(smallerFont);
    ui->labelElapsedTime->setFont(smallerFont);
    ui->labelRemain->setFont(smallerFont);
    ui->labelRemainTime->setFont(smallerFont);
    ui->labelArtistHeader->setFont(smallerFontBold);
    ui->labelArtist->setFont(smallerFont);
    ui->labelTitleHeader->setFont(smallerFontBold);
    ui->labelTitle->setFont(smallerFont);
    ui->labelSingerHeader->setFont(smallerFontBold);
    ui->labelSinger->setFont(smallerFont);

    setFont(font);
    QFontMetrics fm(m_settings.applicationFont());
    int cvwWidth = std::max(300, fm.horizontalAdvance("Total: 00:00  Current:00:00  Remain: 00:00"));
    m_logger->info("{} Resizing videoPreview to width: {}", m_loggingPrefix, cvwWidth);
    QSize mcbSize(fm.height(), fm.height());
    if (mcbSize.width() < 32) {
        mcbSize.setWidth(32);
        mcbSize.setHeight(32);
    }
    ui->buttonBmStop->resize(mcbSize);
    ui->buttonBmPause->resize(mcbSize);
    ui->buttonBmStop->setIconSize(mcbSize);
    ui->buttonBmPause->setIconSize(mcbSize);
    ui->buttonBmStop->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    ui->buttonBmPause->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    autosizeViews();
}

void MainWindow::autosizeRotationCols() {
    if (m_settings.rotationShowNextSong()) {
        ui->tableViewRotation->showColumn(TableModelRotation::COL_NEXT_SONG);
        ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_NAME,
                                                                        QHeaderView::Interactive);
        ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_NEXT_SONG,
                                                                        QHeaderView::Stretch);
    } else {
        ui->tableViewRotation->horizontalHeader()->setSectionResizeMode(TableModelRotation::COL_NAME,
                                                                        QHeaderView::Stretch);
        ui->tableViewRotation->hideColumn(TableModelRotation::COL_NEXT_SONG);
    }
}

void MainWindow::autosizeViews() {
    autosizeKaraokeDbCols();
    autosizeRotationCols();
    autosizeQueueCols();
    autosizeHistoryCols();
}

void MainWindow::autosizeKaraokeDbCols() const
{
    auto *header = ui->tableViewDB->horizontalHeader();

    // Give metadata columns predictable widths.
    // These remain manually resizable.
    struct ColumnWidth {
        int column;
        int width;
    };

    const ColumnWidth fixedColumns[] = {
        {TableModelKaraokeSongs::COL_SONGID,   145},
        {TableModelKaraokeSongs::COL_DURATION,  75},
        {TableModelKaraokeSongs::COL_PLAYS,     75},
        {TableModelKaraokeSongs::COL_LASTPLAY, 170}
    };

    for (const auto &entry : fixedColumns) {
        header->setSectionResizeMode(
            entry.column,
            QHeaderView::Interactive
        );

        header->resizeSection(
            entry.column,
            entry.width
        );
    }

    // Let artist and title share the remaining space, but remain resizable by the user.
    okj::sizeFlexibleColumns(
        ui->tableViewDB,
        TableModelKaraokeSongs::COL_ARTIST,
        TableModelKaraokeSongs::COL_TITLE
    );
}

void MainWindow::applyQueueTabLayout()
{
    if (m_shuttingDown || !ui->tabWidgetQueue->isVisible())
        return;
    if (ui->tabWidgetQueue->currentIndex() == 1) {
        autosizeHistoryCols();
        m_settings.restoreColumnWidths(ui->tableViewHistory);
    } else if (ui->tabWidgetQueue->currentIndex() == 0) {
        autosizeQueueCols();
        m_settings.restoreColumnWidths(ui->tableViewQueue);
    }
}

void MainWindow::autosizeQueueCols()
{
    auto *table = ui->tableViewQueue;
    auto *header = table->horizontalHeader();

    const int fontHeight =
        QFontMetrics(m_settings.applicationFont()).height();

    // Path/status icon column.
    header->setSectionResizeMode(
        TableModelQueueSongs::COL_PATH,
        QHeaderView::Fixed
    );
    header->resizeSection(
        TableModelQueueSongs::COL_PATH,
        fontHeight * 2
    );

    // Predictable widths, even when the queue is empty.
    header->setSectionResizeMode(
        TableModelQueueSongs::COL_SONGID,
        QHeaderView::Interactive
    );
    header->resizeSection(
        TableModelQueueSongs::COL_SONGID, 115
    );

    header->setSectionResizeMode(
        TableModelQueueSongs::COL_DURATION,
        QHeaderView::Interactive
    );
    header->resizeSection(
        TableModelQueueSongs::COL_DURATION, 90
    );

    header->setSectionResizeMode(
        TableModelQueueSongs::COL_KEY,
        QHeaderView::Interactive
    );
    header->resizeSection(
        TableModelQueueSongs::COL_KEY, 65
    );

    // Let artist and title share the remaining space, but remain resizable by the user.
    okj::sizeFlexibleColumns(
        ui->tableViewQueue,
        TableModelQueueSongs::COL_ARTIST,
        TableModelQueueSongs::COL_TITLE
    );
}

void MainWindow::autosizeHistoryCols()
{
    auto *header =
        ui->tableViewHistory->horizontalHeader();

    // Use explicit widths rather than measuring hidden rows.
    const struct {
        int column;
        int width;
    } columns[] = {
        {TableModelHistorySongs::LAST_SUNG, 155},
        {TableModelHistorySongs::SUNG_COUNT, 95},
        {TableModelHistorySongs::KEY_CHANGE, 90},
        {TableModelHistorySongs::SONGID, 115}
    };

    for (const auto &column : columns) {
        header->setSectionResizeMode(
            column.column,
            QHeaderView::Interactive
        );
        header->resizeSection(
            column.column,
            column.width
        );
    }

    // Let artist and title share the remaining space, but remain resizable by the user.
    okj::sizeFlexibleColumns(
        ui->tableViewHistory,
        TableModelHistorySongs::ARTIST,
        TableModelHistorySongs::TITLE
    );
}

void MainWindow::autosizeBmViews() {

    int fH = QFontMetrics(m_settings.applicationFont()).height();
    int iconWidth = fH + fH;
    int durationColSize = QFontMetrics(m_settings.applicationFont()).horizontalAdvance("Duration") + fH;
    // 4 = filename 1 = metadata artist 2 = metadata title

    int artistColSize = 0;
    int titleColSize = 0;
    int fileNameColSize = 0;
    int remainingSpace = ui->tableViewBmDb->width() - durationColSize - 15;
    if (m_settings.bmShowMetadata() && m_settings.bmShowFilenames()) {
        artistColSize = (int) ((float) remainingSpace * .25);
        titleColSize = (int) ((float) remainingSpace * .25);
        fileNameColSize = (int) ((float) remainingSpace * .5);
    } else if (m_settings.bmShowMetadata()) {
        artistColSize = (int) ((float) remainingSpace * .5);
        titleColSize = (int) ((float) remainingSpace * .5);
    } else if (m_settings.bmShowFilenames()) {
        fileNameColSize = remainingSpace;
    }
    ui->tableViewBmDb->horizontalHeader()->resizeSection(TableModelBreakSongs::COL_ARTIST, artistColSize);
    ui->tableViewBmDb->horizontalHeader()->resizeSection(TableModelBreakSongs::COL_TITLE, titleColSize);
    ui->tableViewBmDb->horizontalHeader()->resizeSection(TableModelBreakSongs::COL_FILENAME, fileNameColSize);
    ui->tableViewBmDb->horizontalHeader()->setSectionResizeMode(TableModelBreakSongs::COL_DURATION, QHeaderView::Fixed);
    ui->tableViewBmDb->horizontalHeader()->resizeSection(TableModelBreakSongs::COL_DURATION, durationColSize);


    remainingSpace = ui->tableViewBmPlaylist->width() - durationColSize - (iconWidth * 2) - 15;
    if (m_settings.bmShowMetadata() && m_settings.bmShowFilenames()) {
        artistColSize = (int) ((float) remainingSpace * .25);
        titleColSize = (int) ((float) remainingSpace * .25);
        fileNameColSize = (int) ((float) remainingSpace * .5);
    } else if (m_settings.bmShowMetadata()) {
        artistColSize = (int) ((float) remainingSpace * .5);
        titleColSize = (int) ((float) remainingSpace * .5);
    } else if (m_settings.bmShowFilenames()) {
        fileNameColSize = remainingSpace;
    }
    ui->tableViewBmPlaylist->horizontalHeader()->resizeSection(TableModelPlaylistSongs::COL_ARTIST, artistColSize);
    ui->tableViewBmPlaylist->horizontalHeader()->resizeSection(TableModelPlaylistSongs::COL_TITLE, titleColSize);
    ui->tableViewBmPlaylist->horizontalHeader()->resizeSection(TableModelPlaylistSongs::COL_FILENAME, fileNameColSize);
    ui->tableViewBmPlaylist->horizontalHeader()->resizeSection(TableModelPlaylistSongs::COL_DURATION, durationColSize);
    ui->tableViewBmPlaylist->horizontalHeader()->setSectionResizeMode(TableModelPlaylistSongs::COL_ID,
                                                                      QHeaderView::Fixed);
    ui->tableViewBmPlaylist->horizontalHeader()->resizeSection(TableModelPlaylistSongs::COL_ID, iconWidth);
    ui->tableViewBmPlaylist->horizontalHeader()->setSectionResizeMode(TableModelPlaylistSongs::COL_PATH,
                                                                      QHeaderView::Fixed);
    ui->tableViewBmPlaylist->horizontalHeader()->resizeSection(TableModelPlaylistSongs::COL_PATH, iconWidth);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    if (!m_initialUiSetupDone)
        return;
    QMainWindow::resizeEvent(event);
    autosizeViews();
    autosizeBmViews();
    if (ui->tabWidget->currentIndex() == 0) {
        autosizeViews();
        m_bNeedAutoSize = true;
        m_kNeedAutoSize = false;
    }
    if (ui->tabWidget->currentIndex() == 1) {
        autosizeBmViews();
        m_bNeedAutoSize = false;
        m_kNeedAutoSize = true;
    }
    m_settings.saveWindowState(this);
}

void MainWindow::tabWidgetCurrentChanged(const int &index) {
    if (m_bNeedAutoSize && index == 1) {
        autosizeBmViews();
        m_bNeedAutoSize = false;
    }
    if (m_kNeedAutoSize && index == 0) {
        autosizeViews();
        m_kNeedAutoSize = false;
    }
}

void MainWindow::bmDatabaseAboutToUpdate() {
    // Scanning adds library rows only; keep the current playlist and playback intact.
    while (m_tableModelPlaylists->canFetchMore())
        m_tableModelPlaylists->fetchMore();
    ui->actionPlaylistImport->setEnabled(false);
}

void MainWindow::bmSongMoved(const int &oldPos, const int &newPos) {
    int curPlPos = m_tableModelPlaylistSongs.currentPosition();
    if (oldPos < curPlPos && newPos >= curPlPos)
        curPlPos--;
    else if (oldPos > curPlPos && newPos <= curPlPos)
        curPlPos++;
    else if (oldPos == curPlPos)
        curPlPos = newPos;
    m_tableModelPlaylistSongs.setCurrentPosition(curPlPos);
    auto nextPlSong = m_tableModelPlaylistSongs.getNextPlSong();
    if (!ui->checkBoxBmBreak->isChecked() && nextPlSong.has_value()) {
        ui->labelBmNext->setText(nextPlSong->get().artist + " - " + nextPlSong->get().title);
    } else
        ui->labelBmNext->setText("None - Breaking after current song");
}

void MainWindow::sliderBmPositionPressed() {
    m_logger->trace("{} BM slider down", m_loggingPrefix);
    m_sliderBmPositionPressed = true;
}

void MainWindow::sliderBmPositionReleased() {
    m_mediaBackendBm.setPosition(ui->sliderBmPosition->value());
    m_sliderBmPositionPressed = false;
    m_logger->trace("{} BM slider up.  Position: {}", m_loggingPrefix, ui->sliderBmPosition->value());
}

void MainWindow::sfxButtonPressed() {
    auto *btn = (SoundFxButton *) sender();
    m_mediaBackendSfx.setMedia(btn->buttonData().toString());
    m_mediaBackendSfx.setVolume(ui->sliderVolume->value());
    m_mediaBackendSfx.play();
}

void MainWindow::addSfxButtonPressed() {
#ifdef Q_OS_LINUX
    QString path = QFileDialog::getOpenFileName(
            this,
            "Select audio file",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            "Audio (*.mp3 *.ogg *.wav *.wma)",
            nullptr,
            QFileDialog::DontUseNativeDialog
    );
#else
    QString path = QFileDialog::getOpenFileName(
            this,
            "Select audio file",
            QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0),
            "Audio (*.mp3 *.ogg *.wav *.wma)",
            nullptr
    );
#endif
    if (path != "") {
        bool ok;
        QString btnLabel = QInputDialog::getText(this, tr("Button Text"), tr("Enter button text:"), QLineEdit::Normal,
                                             QString(), &ok);
        if (!ok || btnLabel.isEmpty())
            return;
        SfxEntry entry;
        entry.name = btnLabel;
        entry.path = path;
        m_settings.addSfxEntry(entry);
        addSfxButton(path, btnLabel);
    }

}

void MainWindow::stopSfxPlayback() {
    m_mediaBackendSfx.stop(true);
}

void MainWindow::removeSfxButton() {
    SfxEntryList entries = m_settings.getSfxEntries();
    SfxEntryList newEntries;
            foreach (SfxEntry entry, entries) {
            if (entry.name == m_lastRtClickedSfxBtn.name && entry.path == m_lastRtClickedSfxBtn.path)
                continue;
            newEntries.push_back(entry);
        }
    m_settings.setSfxEntries(newEntries);
    refreshSfxButtons();
}

void MainWindow::showAlert(const QString &title, const QString &message) {
    QMessageBox msgBox;
    msgBox.setWindowTitle(title);
    msgBox.setText(message);
    msgBox.exec();
}

void MainWindow::tableViewRotationCurrentChanged(const QModelIndex &cur, const QModelIndex &prev) {
    Q_UNUSED(prev)
    m_qModel.loadSinger(cur.data(Qt::UserRole).toInt());
    m_historySongsModel.loadSinger(m_rotModel.getSinger(cur.data(Qt::UserRole).toInt()).name);
    if (!m_settings.treatAllSingersAsRegs() && !cur.sibling(cur.row(), TableModelRotation::COL_REGULAR).data().toBool())
        ui->tabWidgetQueue->removeTab(1);
    else if (ui->tabWidgetQueue->count() == 1) {
        ui->tabWidgetQueue->addTab(m_historyTabWidget, "History");
    }
    ui->gbxQueue->setTitle(
            QString("Song Queue - " + cur.sibling(cur.row(), TableModelRotation::COL_NAME).data().toString()));
    if (!ui->tabWidgetQueue->isVisible()) {
        ui->tabWidgetQueue->setVisible(true);
        ui->labelNoSinger->setVisible(false);
        m_queueLayout.schedule();
    }
}

void MainWindow::connectDurationScanStatus() {
    connect(m_lazyDurationUpdater.get(), &LazyDurationUpdateController::scanStarted, this, [this] {
        m_durationScanState = DurationScanState::Selecting;
        m_durationScanError.clear();
        m_durationProcessed = m_durationTotal = m_durationUpdated = m_durationMissing = m_durationFailed = 0;
        updateDatabaseStatus();
    });
    connect(m_lazyDurationUpdater.get(), &LazyDurationUpdateController::scanProgress, this,
            [this](qint64 processed, qint64 total, qint64 updated, qint64 missing, qint64 failed) {
        m_durationScanState = DurationScanState::Scanning;
        m_durationProcessed = processed;
        m_durationTotal = total;
        m_durationUpdated = updated;
        m_durationMissing = missing;
        m_durationFailed = failed;
        updateDatabaseStatus();
    });
    connect(m_lazyDurationUpdater.get(), &LazyDurationUpdateController::scanFinished, this,
            [this](bool cancelled, const QString &error) {
        m_durationScanError = error;
        m_durationScanState = !error.isEmpty() ? DurationScanState::Failed :
            cancelled ? DurationScanState::Stopped : DurationScanState::Idle;
        updateDatabaseStatus();
    });
}

void MainWindow::updateDatabaseStatus() {
    if (m_shuttingDown)
        return;
    const auto summary = tr("Duration scan: %1 of %2 checked; %3 updated, %4 inaccessible, %5 failed.")
        .arg(QLocale().toString(m_durationProcessed), QLocale().toString(m_durationTotal),
             QLocale().toString(m_durationUpdated), QLocale().toString(m_durationMissing), QLocale().toString(m_durationFailed));
    if (m_karaokeSongsModel.isLoading() || (requestsDialog && requestsDialog->isLibraryLoading())) {
        m_labelDatabaseStatus.setText(tr("Database: Loading library…"));
        m_labelDatabaseStatus.setToolTip(tr("Loading the song library in the background."));
    } else if (m_libraryLoadFailed) {
        m_labelDatabaseStatus.setText(tr("Database: Load failed"));
        m_labelDatabaseStatus.setToolTip(tr("The song library could not be loaded. See the log for details."));
    } else {
        switch (m_durationScanState) {
        case DurationScanState::Selecting:
            m_labelDatabaseStatus.setText(tr("Database: Finding missing durations…"));
            break;
        case DurationScanState::Scanning:
            m_labelDatabaseStatus.setText(tr("Durations: %1 remaining").arg(QLocale().toString(qMax(qint64(0), m_durationTotal - m_durationProcessed))));
            break;
        case DurationScanState::Stopped:
            m_labelDatabaseStatus.setText(tr("Database: Duration scan stopped"));
            break;
        case DurationScanState::Failed:
            m_labelDatabaseStatus.setText(tr("Database: Duration scan failed"));
            break;
        case DurationScanState::Idle:
            m_labelDatabaseStatus.setText(tr("Database: Ready"));
            break;
        }
        m_labelDatabaseStatus.setToolTip(m_durationScanError.isEmpty() ? summary : m_durationScanError + "\n" + summary);
    }
}

bool MainWindow::updateKaraokeAutoAdvanceTarget() {
    const auto singer = m_rotModel.nextSinger();
    m_kAANextSinger = singer.isValid() ? singer.id : -1;
    m_kAANextSongPath = singer.isValid() ? singer.nextSongPath() : QString();
    cdgWindow->setNextSinger(singer.isValid() ? singer.name : QString());
    cdgWindow->setNextSong(singer.isValid() ? singer.nextSongArtistTitle() : QString());
    return singer.isValid();
}

void MainWindow::updateAutoplayStatus() {
    if (!m_settings.karaokeAutoAdvance()) {
        m_labelAutoplayStatus.setText(tr(" Autoplay: Off"));
        m_labelAutoplayStatus.setToolTip(tr("Enable Autoplay mode from the Karaoke menu to advance through the rotation."));
    } else if (m_timerKaraokeAA.isActive()) {
        const int seconds = (qMax(0, m_timerKaraokeAA.remainingTime()) + 999) / 1000;
        const auto countdown = QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
        m_labelAutoplayStatus.setText(tr(" Autoplay: %1").arg(countdown));
        const auto &singer = m_rotModel.getSinger(m_kAANextSinger);
        m_labelAutoplayStatus.setToolTip(tr("Next: %1 — %2").arg(singer.name, singer.nextSongArtistTitle()));
    } else {
        m_labelAutoplayStatus.setText(tr(" Autoplay: On"));
        m_labelAutoplayStatus.setToolTip(tr("The next-song countdown starts when the current song finishes."));
    }
}

void MainWindow::updateRotationDuration() {
    QString text;
    int secs = m_rotModel.rotationDuration();
    if (secs > 0) {
        int hours = 0;
        int minutes = secs / 60;
        int seconds = secs % 60;
        if (seconds > 0)
            minutes++;
        if (minutes > 60) {
            hours = minutes / 60;
            minutes = minutes % 60;
            if (hours > 1)
                text = " Rotation Duration: " + QString::number(hours) + " hours " + QString::number(minutes) + " min";
            else
                text = " Rotation Duration: " + QString::number(hours) + " hour " + QString::number(minutes) + " min";
        } else
            text = " Rotation Duration: " + QString::number(minutes) + " min";
    } else
        text = " Rotation Duration: 0 min";
    m_labelRotationDuration.setText(text);
}

void MainWindow::rotationSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected) {
    if (selected.empty()) {
        m_logger->trace("{} Rotation Selection Cleared!", m_loggingPrefix);
        m_qModel.loadSinger(-1);
        ui->tableViewRotation->reset();
        ui->gbxQueue->setTitle("Song Queue");
        ui->tabWidgetQueue->setVisible(false);
        ui->labelNoSinger->setVisible(true);
    }
    m_logger->trace("{} Rotation Selection Changed", m_loggingPrefix);
}

void MainWindow::lineEditBmSearchChanged(const QString &arg1) {
    if (!m_settings.progressiveSearchEnabled())
        return;
    static QString lastVal;
    if (arg1.trimmed() != lastVal) {
        m_tableModelBreakSongs.search(arg1);
        lastVal = arg1.trimmed();
    }
}

void MainWindow::btnRotTopClicked() {
    auto indexes = ui->tableViewRotation->selectionModel()->selectedRows();
    std::vector<int> singerIds;
    std::for_each(indexes.begin(), indexes.end(), [&](QModelIndex index) {
        singerIds.emplace_back(index.data(Qt::UserRole).toInt());
    });
    std::for_each(singerIds.rbegin(), singerIds.rend(), [&](auto singerId) {
        m_rotModel.singerMove(m_rotModel.getSinger(singerId).position, 0);
    });
    auto topLeft = ui->tableViewRotation->model()->index(0, 0);
    auto bottomRight = ui->tableViewRotation->model()->index((int) singerIds.size() - 1, m_rotModel.columnCount(QModelIndex()) - 1);
    ui->tableViewRotation->clearSelection();
    ui->tableViewRotation->selectionModel()->select(QItemSelection(topLeft, bottomRight), QItemSelectionModel::Select);
    rotationDataChanged();
}

void MainWindow::btnRotUpClicked() {
    if (ui->tableViewRotation->selectionModel()->selectedRows().count() < 1)
        return;
    int curPos = ui->tableViewRotation->selectionModel()->selectedRows().at(0).row();
    if (curPos == 0)
        return;
    m_rotModel.singerMove(curPos, curPos - 1);
    ui->tableViewRotation->selectRow(curPos - 1);
    rotationDataChanged();
}

void MainWindow::btnRotDownClicked() {
    if (ui->tableViewRotation->selectionModel()->selectedRows().count() < 1)
        return;
    int curPos = ui->tableViewRotation->selectionModel()->selectedRows().at(0).row();
    if (curPos == m_rotModel.singerCount() - 1)
        return;
    m_rotModel.singerMove(curPos, curPos + 1);
    ui->tableViewRotation->selectRow(curPos + 1);
    rotationDataChanged();
}

void MainWindow::btnRotBottomClicked() {
    auto indexes = ui->tableViewRotation->selectionModel()->selectedRows();
    std::vector<int> singerIds;
    std::for_each(indexes.begin(), indexes.end(), [&](QModelIndex index) {
        singerIds.emplace_back(index.data(Qt::UserRole).toInt());
    });
    std::for_each(singerIds.begin(), singerIds.end(), [&](auto singerId) {
        m_rotModel.singerMove(m_rotModel.getSinger(singerId).position, m_rotModel.singerCount() - 1);
    });
    auto topLeft = ui->tableViewRotation->model()->index((int) (m_rotModel.singerCount() - singerIds.size()), 0);
    auto bottomRight = ui->tableViewRotation->model()->index(static_cast<int>(m_rotModel.singerCount() - 1), m_rotModel.columnCount(QModelIndex()) - 1);
    ui->tableViewRotation->clearSelection();
    ui->tableViewRotation->selectionModel()->select(QItemSelection(topLeft, bottomRight), QItemSelectionModel::Select);
    rotationDataChanged();
}

void MainWindow::btnQTopClicked() {
    auto indexes = ui->tableViewQueue->selectionModel()->selectedRows();
    std::vector<int> songIds;
    std::for_each(indexes.begin(), indexes.end(), [&](QModelIndex index) {
        songIds.emplace_back(index.data().toInt());
    });
    std::for_each(songIds.rbegin(), songIds.rend(), [&](auto songId) {
        m_qModel.moveSongId(songId, 0);
    });
    auto topLeft = ui->tableViewQueue->model()->index(0, 0);
    auto bottomRight = ui->tableViewQueue->model()->index((int) songIds.size() - 1, m_qModel.columnCount() - 1);
    ui->tableViewQueue->clearSelection();
    ui->tableViewQueue->selectionModel()->select(QItemSelection(topLeft, bottomRight), QItemSelectionModel::Select);
    rotationDataChanged();
}

void MainWindow::btnQUpClicked() {
    if (ui->tableViewQueue->selectionModel()->selectedRows().count() < 1)
        return;
    int curPos = ui->tableViewQueue->selectionModel()->selectedRows().at(0).row();
    if (curPos == 0)
        return;
    m_qModel.move(curPos, curPos - 1);
    ui->tableViewQueue->selectRow(curPos - 1);
    rotationDataChanged();
}

void MainWindow::btnQDownClicked() {
    if (ui->tableViewQueue->selectionModel()->selectedRows().count() < 1)
        return;
    int curPos = ui->tableViewQueue->selectionModel()->selectedRows().at(0).row();
    if (curPos == ui->tableViewQueue->model()->rowCount() - 1)
        return;
    m_qModel.move(curPos, curPos + 1);
    ui->tableViewQueue->selectRow(curPos + 1);
    rotationDataChanged();
}

void MainWindow::btnQBottomClicked() {
    auto indexes = ui->tableViewQueue->selectionModel()->selectedRows();
    std::vector<int> songIds;
    std::for_each(indexes.begin(), indexes.end(), [&](QModelIndex index) {
        songIds.emplace_back(index.data().toInt());
    });
    std::for_each(songIds.begin(), songIds.end(), [&](auto songId) {
        m_qModel.moveSongId(songId, m_qModel.rowCount() - 1);
    });
    auto topLeft = ui->tableViewQueue->model()->index((int) (m_qModel.rowCount() - songIds.size()), 0);
    auto bottomRight = ui->tableViewQueue->model()->index(m_qModel.rowCount() - 1, m_qModel.columnCount() - 1);
    ui->tableViewQueue->clearSelection();
    ui->tableViewQueue->selectionModel()->select(QItemSelection(topLeft, bottomRight), QItemSelectionModel::Select);
    rotationDataChanged();
}

void MainWindow::btnBmPlRandomizeClicked() {
    if (m_tableModelPlaylistSongs.rowCount() < 2)
        return;
    m_tableModelPlaylistSongs.randomizePlaylist();
}

void MainWindow::btnPlTopClicked() {
    auto indexes = ui->tableViewBmPlaylist->selectionModel()->selectedRows();
    std::vector<int> plSongIds;
    std::for_each(indexes.begin(), indexes.end(), [&](QModelIndex index) {
        plSongIds.emplace_back(index.data().toInt());
    });
    std::for_each(plSongIds.rbegin(), plSongIds.rend(), [&](auto plSongId) {
        m_tableModelPlaylistSongs.moveSong(m_tableModelPlaylistSongs.getSongPositionById(plSongId), 0);
    });
    m_tableModelPlaylistSongs.savePlaylistChanges();
    auto topLeft = ui->tableViewBmPlaylist->model()->index(0, 0);
    auto bottomRight = ui->tableViewBmPlaylist->model()->index((int) plSongIds.size() - 1, 7);
    ui->tableViewBmPlaylist->selectionModel()->select(QItemSelection(topLeft, bottomRight),
                                                      QItemSelectionModel::Select);
}

void MainWindow::btnPlUpClicked() {
    if (ui->tableViewBmPlaylist->selectionModel()->selectedRows().count() < 1)
        return;
    int curPos = ui->tableViewBmPlaylist->selectionModel()->selectedRows().at(0).row();
    if (curPos == 0)
        return;
    m_tableModelPlaylistSongs.moveSong(curPos, curPos - 1);
    m_tableModelPlaylistSongs.savePlaylistChanges();
    ui->tableViewBmPlaylist->selectRow(curPos - 1);
}

void MainWindow::btnPlDownClicked() {
    int maxPos = ui->tableViewBmPlaylist->model()->rowCount() - 1;
    if (ui->tableViewBmPlaylist->selectionModel()->selectedRows().count() < 1)
        return;
    int curPos = ui->tableViewBmPlaylist->selectionModel()->selectedRows().at(0).row();
    if (curPos == maxPos)
        return;
    m_tableModelPlaylistSongs.moveSong(curPos, curPos + 1);
    m_tableModelPlaylistSongs.savePlaylistChanges();
    ui->tableViewBmPlaylist->selectRow(curPos + 1);
}

void MainWindow::btnPlBottomClicked() {
    auto indexes = ui->tableViewBmPlaylist->selectionModel()->selectedRows();
    std::vector<int> plSongIds;
    std::for_each(indexes.begin(), indexes.end(), [&](QModelIndex index) {
        plSongIds.emplace_back(index.data().toInt());
    });
    std::for_each(plSongIds.begin(), plSongIds.end(), [&](auto plSongId) {
        m_tableModelPlaylistSongs.moveSong(m_tableModelPlaylistSongs.getSongPositionById(plSongId),
                                           m_tableModelPlaylistSongs.rowCount() - 1);
    });
    m_tableModelPlaylistSongs.savePlaylistChanges();
    auto topLeft = ui->tableViewBmPlaylist->model()->index(
            (int) (m_tableModelPlaylistSongs.rowCount() - plSongIds.size()), 0);
    auto bottomRight = ui->tableViewBmPlaylist->model()->index(m_tableModelPlaylistSongs.rowCount() - 1, 7);
    ui->tableViewBmPlaylist->selectionModel()->select(QItemSelection(topLeft, bottomRight),
                                                      QItemSelectionModel::Select);
}

void MainWindow::actionSoundClipsTriggered(const bool &checked) {
    if (checked) {
        ui->groupBoxSoundClips->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->scrollAreaSoundClips->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->scrollAreaWidgetContents->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        ui->verticalSpacerRtPanel->changeSize(0, 20, QSizePolicy::Ignored, QSizePolicy::Ignored);
    } else {
        ui->groupBoxSoundClips->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        ui->verticalSpacerRtPanel->changeSize(0, 20, QSizePolicy::Ignored, QSizePolicy::Expanding);
    }
    ui->groupBoxSoundClips->setVisible(checked);
    m_settings.setShowMainWindowSoundClips(checked);
}

void MainWindow::actionNowPlayingTriggered(const bool &checked) {
    ui->groupBoxNowPlaying->setVisible(checked);
    m_settings.setShowMainWindowNowPlaying(checked);
}

void MainWindow::actionVideoSmallTriggered() {
    ui->videoPreview->setMinimumSize(QSize(256, 144));
    ui->videoPreview->setMaximumSize(QSize(256, 144));
    ui->videoPreviewBm->setMinimumSize(QSize(256, 144));
    ui->videoPreviewBm->setMaximumSize(QSize(256, 144));
    ui->mediaFrame->setMaximumWidth(300);
    ui->mediaFrame->setMinimumWidth(300);
    m_settings.setMainWindowVideoSize(Settings::Small);
    if (auto *eqPanel = findChild<DlgEq *>(QStringLiteral("equalizerPanel")))
        eqPanel->setFrequencyLabelsVisible(false);
    QTimer::singleShot(15, [&]() { autosizeViews(); });
}

void MainWindow::actionVideoMediumTriggered() {
    ui->videoPreview->setMinimumSize(QSize(384, 216));
    ui->videoPreview->setMaximumSize(QSize(384, 216));
    ui->videoPreviewBm->setMinimumSize(QSize(384, 216));
    ui->videoPreviewBm->setMaximumSize(QSize(384, 216));
    ui->mediaFrame->setMaximumWidth(430);
    ui->mediaFrame->setMinimumWidth(430);
    m_settings.setMainWindowVideoSize(Settings::Medium);
    if (auto *eqPanel = findChild<DlgEq *>(QStringLiteral("equalizerPanel")))
        eqPanel->setFrequencyLabelsVisible(true);
    QTimer::singleShot(15, [&]() { autosizeViews(); });
}

void MainWindow::actionVideoLargeTriggered() {
    ui->videoPreview->setMinimumSize(QSize(512, 288));
    ui->videoPreview->setMaximumSize(QSize(512, 288));
    ui->videoPreviewBm->setMinimumSize(QSize(512, 288));
    ui->videoPreviewBm->setMaximumSize(QSize(512, 288));
    ui->mediaFrame->setMaximumWidth(560);
    ui->mediaFrame->setMinimumWidth(560);
    m_settings.setMainWindowVideoSize(Settings::Large);
    if (auto *eqPanel = findChild<DlgEq *>(QStringLiteral("equalizerPanel")))
        eqPanel->setFrequencyLabelsVisible(true);
    QTimer::singleShot(15, [&]() { autosizeViews(); });
}

void MainWindow::actionKaraokeTorture() {
    connect(&m_timerTest, &QTimer::timeout, [&]() {
        QApplication::beep();
        static int runs = 0;
        m_logger->info("{} Karaoke torture test timer timeout", m_loggingPrefix);
        m_logger->info("{} num songs in db: {}", m_loggingPrefix, m_karaokeSongsModel.rowCount(QModelIndex()));
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        auto dist = std::uniform_int_distribution<int>(0, m_karaokeSongsModel.rowCount(QModelIndex()) - 1);
        int randomNum = dist(rng);
        randomNum = 1;
        m_logger->info("{} randomNum: {}", m_loggingPrefix, randomNum);
        ui->tableViewDB->selectRow(randomNum);
        ui->tableViewDB->scrollTo(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        play(m_karaokeSongsModel.getPath(
                     ui->tableViewDB->selectionModel()->selectedRows(TableModelKaraokeSongs::COL_ID).at(0).data().toInt()),
             true);
        ui->labelSinger->setText("Torture run (" + QString::number(++runs) + ")");
    });
    m_timerTest.start(4000);
}

void MainWindow::actionKAndBTorture() {
    connect(&m_timerTest, &QTimer::timeout, [&]() {
        QApplication::beep();
        static bool playing = false;
        static int runs = 0;
        if (playing) {
            buttonStopClicked();
            playing = false;
            ui->labelSinger->setText("Torture run (" + QString::number(runs) + ")");
            return;
        }
        m_logger->info("{} Karaoke torture test timer timeout", m_loggingPrefix);
        m_logger->info("{} num songs in db: {}", m_loggingPrefix, m_karaokeSongsModel.rowCount(QModelIndex()));
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        int randomNum = 0;
        if (m_karaokeSongsModel.rowCount(QModelIndex()) > 0) {
            auto dist = std::uniform_int_distribution<int>(0, m_karaokeSongsModel.rowCount(QModelIndex()) - 1);
            randomNum = dist(rng);
        }
        m_logger->info("{} randomNum: {}", m_loggingPrefix, randomNum);
        ui->tableViewDB->selectRow(randomNum);
        ui->tableViewDB->scrollTo(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        play(ui->tableViewDB->selectionModel()->selectedRows(5).at(0).data().toString(), false);
        ui->labelSinger->setText("Torture run (" + QString::number(++runs) + ")");
        playing = true;
    });
    m_timerTest.start(2000);
}

void MainWindow::actionBurnIn() {
    m_testMode = true;
    emit ui->buttonClearRotation->clicked();
    for (auto i = 0; i < 21; i++) {
        auto singerName = "Test Singer " + QString::number(i);
        m_rotModel.singerAdd(singerName);
    }
    connect(&m_timerTest, &QTimer::timeout, [&]() {
        QApplication::beep();
        static bool playing = false;
        static int runs = 0;
        if (playing) {
            buttonStopClicked();
            playing = false;
            ui->labelSinger->setText("Torture run (" + QString::number(runs) + ")");
            return;
        }
        auto dist = std::uniform_int_distribution<int>(0, 19);
        m_rotModel.singerMove(dist(rng), dist(rng));
        ui->tableViewRotation->selectRow(dist(rng));
        int randomNum{0};
        if (m_karaokeSongsModel.rowCount(QModelIndex()) > 1) {
            dist = std::uniform_int_distribution<int>(0, m_karaokeSongsModel.rowCount(QModelIndex()) - 1);
            randomNum = dist(rng);
        }
        m_logger->info("{} randomNum: {}", m_loggingPrefix, randomNum);
        ui->tableViewDB->selectRow(randomNum);
        ui->tableViewDB->scrollTo(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        emit ui->tableViewDB->doubleClicked(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        ui->tableViewQueue->selectRow(m_qModel.rowCount() - 1);
        ui->tableViewQueue->scrollTo(ui->tableViewQueue->selectionModel()->selectedRows().at(0));
        if (m_qModel.rowCount() > 2) {
            dist = std::uniform_int_distribution<int>(0, m_qModel.rowCount() - 1);
            int newPos = dist(rng);
            m_qModel.move(m_qModel.rowCount() - 1, newPos);
            ui->tableViewQueue->selectRow(newPos);
            ui->tableViewQueue->scrollTo(ui->tableViewQueue->selectionModel()->selectedRows().at(0));
        }
        auto idx = ui->tableViewQueue->selectionModel()->selectedRows().at(0);
        emit ui->tableViewQueue->doubleClicked(idx);
        ui->tableViewQueue->selectRow(idx.row());
        playing = true;
        m_logger->info("{} Burn in test cycle: {}", m_loggingPrefix, ++runs);
    });
    m_timerTest.start(3000);
}

void MainWindow::actionPreviewBurnIn() {
    m_testMode = true;
    connect(&m_timerTest, &QTimer::timeout, [&]() {
        QApplication::beep();
        static bool playing = false;
        static int runs = 0;
        ui->labelSinger->setText("Preview burn-in run (" + QString::number(runs) + ")");
        int randomNum{0};
        if (m_karaokeSongsModel.rowCount(QModelIndex()) > 1)
        {
            auto dist = std::uniform_int_distribution<int>(0, m_karaokeSongsModel.rowCount(QModelIndex()) - 1);
            randomNum = dist(rng);
        }
        m_logger->info("{} randomNum: {}", m_loggingPrefix, randomNum);
        ui->tableViewDB->selectRow(randomNum);
        ui->tableViewDB->scrollTo(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        auto selRow = ui->tableViewDB->selectionModel()->selectedRows().at(0);
        int songId = selRow.sibling(selRow.row(), TableModelKaraokeSongs::COL_ID).data().toInt();
        previewKaraokeSong(m_karaokeSongsModel.getPath(songId));
        m_logger->info("{} Preview burn-in test cycle: {}", m_loggingPrefix, ++runs);
    });
    m_timerTest.start(3250);
}

void MainWindow::actionCdgDecodeTorture() {
    connect(&m_timerTest, &QTimer::timeout, [&]() {
        QApplication::beep();
        static int runs = 0;
        m_logger->info("{} Karaoke torture test timer timeout", m_loggingPrefix);
        m_logger->info("{} num songs in db: {}", m_loggingPrefix, m_karaokeSongsModel.rowCount(QModelIndex()));
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        ui->tableViewDB->scrollToBottom();
        int randomNum{0};
        if (m_karaokeSongsModel.rowCount(QModelIndex()) > 0)
        {
            auto dist = std::uniform_int_distribution<int>(0, m_karaokeSongsModel.rowCount(QModelIndex()) - 1);
            randomNum = dist(rng);
        }
        m_logger->info("{} randomNum: {}", m_loggingPrefix, randomNum);
        ui->tableViewDB->selectRow(randomNum);
        ui->tableViewDB->scrollTo(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        QString karaokeFilePath = ui->tableViewDB->selectionModel()->selectedRows(5).at(0).data().toString();
        if (!karaokeFilePath.endsWith("zip", Qt::CaseInsensitive) &&
            !karaokeFilePath.endsWith("cdg", Qt::CaseInsensitive)) {
            return;
        }
        if (karaokeFilePath.endsWith(".zip", Qt::CaseInsensitive)) {
            MzArchive archive(karaokeFilePath);
            if ((archive.checkCDG()) && (archive.checkAudio())) {
                if (archive.checkAudio()) {
                    if (!archive.extractAudio(m_mediaTempDir->path(), "tmp" + archive.audioExtension())) {
                        return;
                    }
                    if (!archive.extractCdg(m_mediaTempDir->path(), "tmp.cdg")) {
                        return;
                    }
                    QString audioFile = m_mediaTempDir->path() + QDir::separator() + "tmp" + archive.audioExtension();
                    QString cdgFile = m_mediaTempDir->path() + QDir::separator() + "tmp.cdg";
                    m_logger->info("{} Extracted audio file size: {}", m_loggingPrefix, QFileInfo(audioFile).size());
                    m_logger->info("{} Setting karaoke backend source file to: {}", m_loggingPrefix,
                                   audioFile.toStdString());
                    m_mediaBackendKar.setMediaCdg(cdgFile, audioFile);
                    //m_mediaBackendKar.testCdgDecode(); // todo: andth
                }
            } else {
                return;
            }
        } else if (karaokeFilePath.endsWith(".cdg", Qt::CaseInsensitive)) {
            QString cdgTmpFile = "tmp.cdg";
            QString audTmpFile = "tmp.mp3";
            QFile cdgFile(karaokeFilePath);
            if (!cdgFile.exists() || cdgFile.size() == 0) {
                return;
            }
            QString audioFilename = findMatchingAudioFile(karaokeFilePath);
            if (audioFilename == "") {
                return;
            }
            QFile audioFile(audioFilename);
            if (audioFile.size() == 0) {
                return;
            }
            cdgFile.copy(m_mediaTempDir->path() + QDir::separator() + cdgTmpFile);
            QFile::copy(audioFilename, m_mediaTempDir->path() + QDir::separator() + audTmpFile);
            m_mediaBackendKar.setMediaCdg(m_mediaTempDir->path() + QDir::separator() + cdgTmpFile,
                                          m_mediaTempDir->path() + QDir::separator() + audTmpFile);
            // m_mediaBackendKar.testCdgDecode(); // todo: andth
        }
        ui->labelSinger->setText("Torture run (" + QString::number(++runs) + ")");
    });
    m_timerTest.start(2000);
}

void MainWindow::writeGstPipelineDiagramToDisk() {
    QString outputFolder = QStandardPaths::standardLocations(QStandardPaths::PicturesLocation).at(0);
    m_mediaBackendKar.writePipelinesGraphToFile(outputFolder);
    m_mediaBackendBm.writePipelinesGraphToFile(outputFolder);
    m_mediaBackendSfx.writePipelinesGraphToFile(outputFolder);
}

void MainWindow::comboBoxSearchTypeIndexChanged(int index) {
    switch (index) {
        case 1:
            m_karaokeSongsModel.setSearchType(TableModelKaraokeSongs::SEARCH_TYPE_ARTIST);
            break;
        case 2:
            m_karaokeSongsModel.setSearchType(TableModelKaraokeSongs::SEARCH_TYPE_TITLE);
            break;
        default:
            m_karaokeSongsModel.setSearchType(TableModelKaraokeSongs::SEARCH_TYPE_ALL);
            break;
    }
}

void MainWindow::actionDocumentation() {
    QDesktopServices::openUrl(QUrl("https://docs.openkj.org"));
}

void MainWindow::buttonHistoryPlayClicked()
{
    const auto rows = ui->tableViewHistory->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return;
    const auto index = rows.first();
    const auto path = index.siblingAtColumn(TableModelHistorySongs::PATH).data().toString();
    const auto singer = m_rotModel.getSingerByName(m_historySongsModel.currentSingerName());
    requestKaraokePlayback({
        .song = {.id = -1, .singerId = singer.id, .dbSongId = m_karaokeSongsModel.getIdForPath(path),
                 .keyChange = index.siblingAtColumn(TableModelHistorySongs::KEY_CHANGE).data().toInt(),
                 .artist = index.siblingAtColumn(TableModelHistorySongs::ARTIST).data().toString(),
                 .title = index.siblingAtColumn(TableModelHistorySongs::TITLE).data().toString(),
                 .songId = index.siblingAtColumn(TableModelHistorySongs::SONGID).data().toString(), .path = path}
    }, InterruptionWarning::Playing);
}

void MainWindow::buttonHistoryToQueueClicked() {
    auto selRows = ui->tableViewHistory->selectionModel()->selectedRows();
    if (selRows.empty())
        return;

    std::for_each(selRows.begin(), selRows.end(), [&](auto index) {
        auto path = index.sibling(index.row(), 2).data().toString();
        int curSingerId = m_rotModel.getSingerByName(m_historySongsModel.currentSingerName()).id;
        int key = index.sibling(index.row(), 6).data().toInt();
        int dbSongId = m_karaokeSongsModel.getIdForPath(path);
        if (dbSongId == -1) {
            QMessageBox::warning(this,
                                 "Unable to add history song",
                                 "Unable to find a matching song in the database for the selected "
                                 "history song.  This usually happens when a file that was once in "
                                 "OpenKJ's database has been removed or renamed outside of OpenKJ\n\n"
                                 "Song: " + path
            );
            return;
        }
        m_qModel.songAddSlot(dbSongId, curSingerId, key);
    });
    ui->tableViewHistory->clearSelection();
    ui->tabWidgetQueue->setCurrentIndex(0);
}

void MainWindow::tableViewHistoryDoubleClicked([[maybe_unused]]const QModelIndex &index) {
    switch (ui->comboBoxHistoryDblClick->currentIndex()) {
        case 0:
            buttonHistoryToQueueClicked();
            break;
        case 1:
            buttonHistoryPlayClicked();
            break;
    }
}

void MainWindow::tableViewHistoryContextMenu(const QPoint &pos) {
    int selCount = ui->tableViewHistory->selectionModel()->selectedRows().size();
    if (selCount == 1) {
        QModelIndex index = ui->tableViewHistory->indexAt(pos);
        if (index.isValid()) {
            QMenu contextMenu(this);
            contextMenu.addAction("Preview", [&]() {
                QString filename = index.sibling(index.row(), 2).data().toString();
                if (!QFile::exists(filename)) {
                    QMessageBox::warning(this, tr("Missing File!"),
                                         "Specified karaoke file missing, preview aborted!\n\n" + m_dbRtClickFile,
                                         QMessageBox::Ok);
                    return;
                }
                auto *videoPreview = new DlgVideoPreview(filename, this);
                videoPreview->setAttribute(Qt::WA_DeleteOnClose);
                videoPreview->show();
            });
            contextMenu.addAction("Play", this, &MainWindow::buttonHistoryPlayClicked);
            contextMenu.addAction("Add to queue", this, &MainWindow::buttonHistoryToQueueClicked);
            contextMenu.addSeparator();
            contextMenu.addAction("Delete", [&]() {
                QMessageBox msgBox;
                msgBox.setText("Delete song from singer history?");
                msgBox.setIcon(QMessageBox::Warning);
                msgBox.setInformativeText("Are you sure you want to remove this song from the singer's history?");
                msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
                msgBox.setDefaultButton(QMessageBox::Cancel);
                int ret = msgBox.exec();
                if (ret == QMessageBox::Cancel)
                    return;
                int songIndex = index.sibling(index.row(), 0).data().toInt();
                m_historySongsModel.deleteSong(songIndex);
                ui->tableViewHistory->clearSelection();
            });
            contextMenu.exec(QCursor::pos());
        }
    } else if (selCount > 1) {
        QMenu contextMenu(this);
        contextMenu.addAction("Add to queue", this, &MainWindow::buttonHistoryToQueueClicked);
        contextMenu.addSeparator();
        contextMenu.addAction("Delete", [&]() {
            QMessageBox msgBox;
            msgBox.setText("Delete songs from singer history?");
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setInformativeText("Are you sure you want to remove these songs from the singer's history?");
            msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
            msgBox.setDefaultButton(QMessageBox::Cancel);
            int ret = msgBox.exec();
            if (ret == QMessageBox::Cancel)
                return;
            auto indexes = ui->tableViewHistory->selectionModel()->selectedRows();
            std::for_each(indexes.rbegin(), indexes.rend(), [&](QModelIndex index) {
                m_historySongsModel.deleteSong(index.data().toInt());
            });
            ui->tableViewHistory->clearSelection();
        });

        contextMenu.exec(QCursor::pos());
    }
}

void MainWindow::actionBreakMusicTorture() {
    m_tableModelPlaylists->select();
    bmAddPlaylist("torture");
    m_tableModelPlaylists->select();
    ui->comboBoxBmPlaylists->setCurrentText("torture");
    ui->tableViewBmDb->selectAll();
    auto mimeData = m_tableModelBreakSongs.mimeData(ui->tableViewBmDb->selectionModel()->selectedIndexes());
    auto throwaway = m_tableModelPlaylistSongs.dropMimeData(mimeData, Qt::CopyAction, 0, 3, QModelIndex());

    connect(&m_timerTest, &QTimer::timeout, [&]() {
        QApplication::beep();
        static int runs = 0;
        m_logger->info("{} Karaoke torture test timer timeout", m_loggingPrefix);
        ui->tableViewBmPlaylist->selectRow(0);
        tableViewBmPlaylistDoubleClicked(ui->tableViewBmPlaylist->selectionModel()->selectedRows().at(0));
        m_logger->info("{} test runs: {}", m_loggingPrefix, ++runs);
    });
    m_timerTest.start(2000);
}

void MainWindow::tableViewBmDbClicked([[maybe_unused]]const QModelIndex &index) {
#ifdef Q_OS_WIN
    ui->tableViewBmPlaylist->setAttribute(Qt::WA_AcceptDrops, false);
    ui->tableViewBmPlaylist->setAttribute(Qt::WA_AcceptDrops, true);
#endif
}

void MainWindow::resetBmLabels() {
    ui->labelBmPlaying->setText("None");
    ui->labelBmNext->setText("None");
    ui->labelBmDuration->setText("00:00");
    ui->labelBmRemaining->setText("00:00");
    ui->labelBmPosition->setText("00:00");
    ui->sliderBmPosition->setValue(0);
}

void MainWindow::actionBurnInEosJump() {
    m_testMode = true;
    emit ui->buttonClearRotation->clicked();
    for (auto i = 0; i < 21; i++) {
        auto singerName = "Test Singer " + QString::number(i);
        m_rotModel.singerAdd(singerName);
        // m_rotModel.regularDelete(singerName);
    }
    connect(&m_timerTest, &QTimer::timeout, [&]() {
        QApplication::beep();
        static bool playing = false;
        static int runs = 0;
        auto dist = std::uniform_int_distribution<int>(0, 19);
        m_rotModel.singerMove(dist(rng), dist(rng));
        ui->tableViewRotation->selectRow(dist(rng));
        int randomNum{0};
        if (m_karaokeSongsModel.rowCount(QModelIndex()) > 1)
        {
            dist = std::uniform_int_distribution<int>(0, m_karaokeSongsModel.rowCount(QModelIndex()) - 1);
            randomNum = dist(rng);
        }
        m_logger->info("{} randomNum: {}", m_loggingPrefix, randomNum);
        ui->tableViewDB->selectRow(randomNum);
        ui->tableViewDB->scrollTo(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        emit ui->tableViewDB->doubleClicked(ui->tableViewDB->selectionModel()->selectedRows().at(0));
        ui->tableViewQueue->selectRow(m_qModel.rowCount() - 1);
        ui->tableViewQueue->scrollTo(ui->tableViewQueue->selectionModel()->selectedRows().at(0));
        if (m_qModel.rowCount() > 2) {
            dist = std::uniform_int_distribution<int>(0, m_qModel.rowCount() - 1);
            int newPos = dist(rng);
            m_qModel.move(m_qModel.rowCount() - 1, newPos);
            ui->tableViewQueue->selectRow(newPos);
            ui->tableViewQueue->scrollTo(ui->tableViewQueue->selectionModel()->selectedRows().at(0));
        }
        auto idx = ui->tableViewQueue->selectionModel()->selectedRows().at(0);
        emit ui->tableViewQueue->doubleClicked(idx);
        ui->tableViewQueue->selectRow(idx.row());
        playing = true;
        QTimer::singleShot(500, [&]() {
            auto duration = m_mediaBackendKar.duration();
            auto jumpPoint = duration - 10000;
            m_mediaBackendKar.setPosition(jumpPoint);
        });
        m_logger->info("{} Burn in test cycle: {}", m_loggingPrefix, ++runs);
        ui->labelSinger->setText("Torture run (" + QString::number(runs) + ")");
    });
    m_timerTest.start(13000);
}

void MainWindow::showAddSingerDialog() {
    auto dlgAddSinger = findChild<DlgAddSinger *>(QString(), Qt::FindDirectChildrenOnly);
    if (dlgAddSinger == nullptr)
    {
        dlgAddSinger = new DlgAddSinger(m_rotModel, this);
        connect(dlgAddSinger, &DlgAddSinger::finished, dlgAddSinger, &DlgAddSinger::deleteLater);
        connect(dlgAddSinger, &DlgAddSinger::newSingerAdded, [&](auto pos) {
            ui->tableViewRotation->selectRow(pos);
            ui->lineEdit->setFocus();
        });
        dlgAddSinger->show();
    } else
        dlgAddSinger->raise();
}


