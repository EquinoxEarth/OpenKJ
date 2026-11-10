#ifndef STARTUPSPLASH_H
#define STARTUPSPLASH_H

#include <QElapsedTimer>
#include <QSplashScreen>
#include <QTimer>

class StartupSplash final : public QSplashScreen
{
public:
    StartupSplash();
    void finishWhenReady(QWidget *mainWindow);

protected:
    void drawContents(QPainter *painter) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QElapsedTimer m_visibleTime;
    QTimer m_animationTimer;
    QRectF m_progressTrack;
};

#endif // STARTUPSPLASH_H
