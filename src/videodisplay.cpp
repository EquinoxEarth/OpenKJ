#include "videodisplay.h"
#include <QPainter>
#include <QPaintEvent>
#include <QSvgRenderer>
#include "xbrz.h"

VideoDisplay::VideoDisplay(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    auto palette = this->palette();
    palette.setColor(QPalette::Window, Qt::black);
    setPalette(palette);
    setMouseTracking(true);

    m_videoSink = new QVideoSink(this);
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, &VideoDisplay::onVideoFrameChanged);
}

void VideoDisplay::onVideoFrameChanged(const QVideoFrame &frame)
{
    if (frame.isValid()) {
        updateFrame(frame.toImage());
    }
}


void VideoDisplay::setBackground(const QPixmap &pixmap)
{
    m_useDefaultBg = false;
    m_currentBg = pixmap;
    update();
}

void VideoDisplay::useDefaultBackground()
{
    m_useDefaultBg = true;
    update();
}

void VideoDisplay::setHasActiveVideo(const bool &value)
{
    if (m_hasActiveVideo != value)
    {
        m_hasActiveVideo = value;
        if (!m_hasActiveVideo)
        {
            m_currentFrame = QImage();
        }
        update();
    }
}

void VideoDisplay::updateFrame(const QImage &image)
{
    m_currentFrame = image;
    m_hasActiveVideo = true;
    update();
}

void VideoDisplay::clearFrame()
{
    m_currentFrame = QImage();
    m_hasActiveVideo = false;
    update();
}

void VideoDisplay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    if (m_hasActiveVideo && !m_currentFrame.isNull())
    {
        painter.fillRect(event->rect(), Qt::black);
        QImage scaled;
        if (m_currentFrame.width() == 300 && m_currentFrame.height() == 216 && m_settings.cdgPrescalingEnabled())
        {
            QSize targetSize = m_currentFrame.size().scaled(size(), Qt::KeepAspectRatio);
            int factor = std::min(targetSize.width() / 300, targetSize.height() / 216);
            if (factor > 6) factor = 6;
            if (factor >= 2)
            {
                QImage src = m_currentFrame.convertToFormat(QImage::Format_ARGB32);
                QImage dst(src.width() * factor, src.height() * factor, QImage::Format_ARGB32);
                xbrz::scale(factor,
                            reinterpret_cast<const uint32_t*>(src.constBits()),
                            reinterpret_cast<uint32_t*>(dst.bits()),
                            src.width(), src.height(),
                            xbrz::ColorFormat::ARGB);
                scaled = dst.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            else
            {
                scaled = m_currentFrame.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        else
        {
            scaled = m_currentFrame.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        painter.drawImage(x, y, scaled);
    }
    else
    {
        // stopped - draw background image
        painter.fillRect(event->rect(), Qt::black);
        if (m_useDefaultBg)
        {
            QSvgRenderer renderer(QString(":icons/Icons/okjlogo.svg"));
            renderer.setAspectRatioMode(Qt::KeepAspectRatio);
            renderer.render(&painter);
        }
        else
        {
            painter.drawPixmap(rect(), m_currentBg, m_currentBg.rect());
        }
    }
}
