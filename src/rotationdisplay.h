#ifndef ROTATIONDISPLAY_H
#define ROTATIONDISPLAY_H

#include "settings.h"
#include <QWidget>

class QAction;
class TableModelRotation;
class VideoDisplay;

class RotationDisplay : public QWidget
{
    Q_OBJECT
public:
    explicit RotationDisplay(TableModelRotation &rotation, QWidget *parent = nullptr);
    [[nodiscard]] const QStringList &singerNames() const { return m_names; }
    [[nodiscard]] QString currentSingerName() const;
    [[nodiscard]] VideoDisplay *videoDisplay() const { return m_videoDisplay; }
    void saveForShutdown();

public slots:
    void refresh();
    void applySettings();
    void setDisplayVisible(bool visible);
    void setFullscreen(bool fullscreen);
    void setCurrentSinger(const QString &name, bool singing);

signals:
    void visibilityChanged(bool visible);

protected:
    void paintEvent(QPaintEvent *) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    TableModelRotation &m_rotation;
    Settings m_settings;
    QStringList m_names;
    QString m_currentSinger;
    QColor m_textColor;
    QColor m_backgroundColor;
    VideoDisplay *m_videoDisplay{nullptr};
    QRectF m_singerArea;
    bool m_showHeading{true};
    bool m_shuttingDown{false};
    bool m_changingState{false};
    QByteArray m_windowedGeometry;
    QAction *m_fullscreenAction{nullptr};
    void ensureOnScreen();
    void rememberWindowedGeometry();
    void saveWindowedGeometry();
    void updateContentGeometry();
};

#endif
