#include "videodisplay.h"
#include <QPainter>
#include <QPaintEvent>
#include <QSvgRenderer>
#include "xbrz.h"
#include <algorithm>
#include <cstdint>

QImage VideoDisplayFrame::scaled(const QSize &bounds, bool prescaleCdg)
{
    const QSize targetSize = m_image.size().scaled(bounds, Qt::KeepAspectRatio);
    int factor = 1;
    if (prescaleCdg && isCdgSized())
        factor = std::clamp(std::min(targetSize.width() / 300, targetSize.height() / 216), 1, 6);

    const auto key = std::pair{targetSize, factor};
    if (const auto *cached = m_scaled.object(key))
        return *cached;

    QImage scaled;
    if (factor >= 2) {
        auto &enlarged = m_enlarged[factor - 2];
        if (enlarged.isNull()) {
            if (m_argb.isNull())
                m_argb = m_image.convertToFormat(QImage::Format_ARGB32);
            QImage output(m_image.width() * factor, m_image.height() * factor, QImage::Format_ARGB32);
            if (!m_argb.isNull() && !output.isNull()) {
                xbrz::scale(factor,
                            reinterpret_cast<const uint32_t*>(m_argb.constBits()),
                            reinterpret_cast<uint32_t*>(output.bits()),
                            m_argb.width(), m_argb.height(), xbrz::ColorFormat::ARGB);
                enlarged = std::move(output);
            }
        }
        scaled = enlarged.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else {
        scaled = m_image.scaled(bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    if (!scaled.isNull())
        m_scaled.insert(key, new QImage(scaled), scaled.sizeInBytes());
    return scaled;
}

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
            m_currentFrame.reset();
        }
        update();
    }
}

void VideoDisplay::updateFrame(const QImage &image)
{
    updateSharedFrame(image.isNull() ? nullptr : std::make_shared<VideoDisplayFrame>(image));
}

void VideoDisplay::updateSharedFrame(const std::shared_ptr<VideoDisplayFrame> &frame)
{
    m_currentFrame = frame;
    m_hasActiveVideo = true;
    update();
}

void VideoDisplay::clearFrame()
{
    m_currentFrame.reset();
    m_hasActiveVideo = false;
    update();
}

void VideoDisplay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    if (m_hasActiveVideo && m_currentFrame)
    {
        painter.fillRect(event->rect(), Qt::black);
        const bool prescale = m_currentFrame->isCdgSized() && m_settings.cdgPrescalingEnabled();
        const auto scaled = m_currentFrame->scaled(size(), prescale);
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
