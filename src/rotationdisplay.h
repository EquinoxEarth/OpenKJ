#ifndef ROTATIONDISPLAY_H
#define ROTATIONDISPLAY_H

#include "settings.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QPixmap>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>
#include <QVariantAnimation>
#include <QVideoFrame>
#include <QWidget>

class QAction;
class QBuffer;
class QLabel;
class QPainter;
class TableModelRotation;
class VideoDisplay;
class QMediaPlayer;
class QWindow;

class RotationDisplay : public QWidget
{
    Q_OBJECT
public:
    explicit RotationDisplay(TableModelRotation &rotation, QWidget *parent = nullptr);
    [[nodiscard]] const QStringList &singerNames() const { return m_names; }
    [[nodiscard]] QString currentSingerName() const;
    [[nodiscard]] VideoDisplay *videoDisplay() const { return m_videoDisplay; }
    void saveForShutdown();
    void updateSummary(int durationSeconds);

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
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    TableModelRotation &m_rotation;
    Settings m_settings;
    QStringList m_names;
    QString m_currentSinger;
    QColor m_textColor;
    int m_rowAlpha{0};
    int m_firstRowAlpha{0};
    QColor m_accentColor;
    QColor m_backgroundColor;
    int m_backgroundDimAlpha{0};
    QString m_backgroundImagePath;
    QPixmap m_backgroundImage;
    QVideoFrame m_pendingBackgroundFrame;
    QThreadPool m_backgroundFramePool;
    QFutureWatcher<QImage> m_backgroundFrameWatcher{this};
    bool m_backgroundFramePreparing{false};
    QPixmap m_readyBackgroundFrame;
    quint64 m_backgroundGeneration{0};
    quint64 m_preparingBackgroundGeneration{0};
    bool m_backgroundIsVideo{false};
    QMediaPlayer *m_backgroundPlayer{nullptr};
    QBuffer *m_backgroundBuffer{nullptr};
    QFutureWatcher<QByteArray> m_backgroundLoadWatcher{this};
    QUrl m_loadingBackgroundSource;
    VideoDisplay *m_videoDisplay{nullptr};
    QLabel *m_summary{nullptr};
    class TickerStrip;
    TickerStrip *m_tickerStrip{nullptr};
    QRectF m_singerArea;
    QRectF m_tickerCurrentArea;
    QRectF m_tickerHeadingArea;
    bool m_ticker{false};
    struct TickerCard {
        qreal offset;
        qreal width;
        QString number;
        QString name;
    };
    QList<TickerCard> m_tickerCards;
    qreal m_tickerCycleWidth{0};
    qreal m_tickerNumberWidth{0};
    QFont m_tickerFont;
    QFont m_tickerFirstFont;
    QVariantAnimation m_tickerAnimation{this};
    const bool m_syncTickerToDisplay;
    QPointer<QWindow> m_tickerWindow;
    QElapsedTimer m_tickerRefreshClock;
    qreal m_tickerOffset{0};
    bool m_showHeading{true};
    bool m_hideNameNotes{false};
    bool m_shuttingDown{false};
    bool m_changingState{false};
    QByteArray m_windowedGeometry;
    QAction *m_fullscreenAction{nullptr};
    void ensureOnScreen();
    void rememberWindowedGeometry();
    void saveWindowedGeometry();
    void updateContentGeometry();
    void updateBackgroundPlayback();
    void setBackgroundVideoSource(const QUrl &source, QByteArray data = {});
    void prepareBackgroundFrame();
    void discardPendingBackgroundFrames();
    bool presentBackgroundFrame();
    void rebuildTickerCards();
    void updateTickerAnimation();
    void paintTickerHeadings(QPainter &painter, const QPaintEvent &event) const;
    void paintTickerCards(QPainter &painter, const QRectF &area) const;
    void drawSingerCard(QPainter &painter, const QRectF &rect, qreal radius, qreal inset,
                        qreal numberWidth, const QString &number, const QString &name, bool first) const;
    [[nodiscard]] int maxFontPixels() const;
    [[nodiscard]] QString displayName(const QString &name) const;
};

#endif
