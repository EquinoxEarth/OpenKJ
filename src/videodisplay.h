#ifndef CDGDISPLAY_H
#define CDGDISPLAY_H

#include "settings.h"
#include <QWidget>
#include <QCache>
#include <QBoxLayout>
#include <QResizeEvent>
#include <QImage>
#include <QVideoSink>
#include <QVideoFrame>
#include <array>
#include <memory>
#include <utility>

// Shared by displays on the GUI thread for the lifetime of one decoded frame.
class VideoDisplayFrame
{
public:
    explicit VideoDisplayFrame(const QImage &image) : m_image(image) {}
    [[nodiscard]] bool isCdgSized() const { return m_image.size() == QSize(300, 216); }
    QImage scaled(const QSize &bounds, bool prescaleCdg);

private:
    QImage m_image;
    QImage m_argb;
    std::array<QImage, 5> m_enlarged; // xBRZ factors 2 through 6
    QCache<std::pair<QSize, int>, QImage> m_scaled{32 * 1024 * 1024};
};

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
    bool m_transparentIdleBackground{false};
    bool m_transparentFramePadding{false};
    std::shared_ptr<VideoDisplayFrame> m_currentFrame;
    QVideoSink *m_videoSink;
    void updateBackgroundAttributes();

public:
    explicit VideoDisplay(QWidget *parent = nullptr);
    [[nodiscard]] bool hasActiveVideo() const { return m_hasActiveVideo; }
    QVideoSink* videoSink() const { return m_videoSink; }
    void updateSharedFrame(const std::shared_ptr<VideoDisplayFrame> &frame);
    void setTransparentIdleBackground(bool enabled);
    void setTransparentFramePadding(bool enabled);

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
