#ifndef CDGDISPLAY_H
#define CDGDISPLAY_H

#include "settings.h"
#include <QWidget>
#include <QBoxLayout>
#include <QResizeEvent>
#include <QImage>
#include <QVideoSink>
#include <QVideoFrame>

class VideoDisplay : public QWidget
{
    Q_OBJECT
private:
    Settings m_settings{this};
    QPixmap m_currentBg;
    bool m_useDefaultBg{true};
    bool m_hasActiveVideo { false };
    bool m_fillOnPaint { false };
    bool m_repaintBackgroundOnce { false };
    QImage m_currentFrame;
    QVideoSink *m_videoSink;

public:
    explicit VideoDisplay(QWidget *parent = nullptr);
    [[nodiscard]] bool hasActiveVideo() const { return m_hasActiveVideo; }
    QVideoSink* videoSink() const { return m_videoSink; }

signals:
    void mouseMoveEvent(QMouseEvent *event) override;

public slots:
    void setBackground(const QPixmap &pixmap);
    void useDefaultBackground();
    void setHasActiveVideo(const bool &value);
    void updateFrame(const QImage &image);
    void clearFrame();
    void onVideoFrameChanged(const QVideoFrame &frame);

    /**
     * @brief Fill with black on paint event when video is playing.
     * Video in HW mode seems to only paint black borders when the control is resized.
     * This causes some glitches in the monitor window when changing between tabs.
     * Set this property to start each paint event with a black fill.
     */
    void setFillOnPaint(const bool &value) { m_fillOnPaint = value; }
protected:
    void paintEvent(QPaintEvent *event) override;
};


#endif // CDGDISPLAY_H
