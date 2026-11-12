#include "videodisplay.h"
#include <QFileInfo>
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

bool VideoDisplay::isBackgroundVideo(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == "mp4" || suffix == "mov" || suffix == "m4v";
}

void VideoDisplay::useDefaultBackground()
{
    m_useDefaultBg = true;
    update();
}

void VideoDisplay::setTransparentIdleBackground(bool enabled)
{
    if (m_transparentIdleBackground == enabled)
        return;
    m_transparentIdleBackground = enabled;
    updateBackgroundAttributes();
    update();
}

void VideoDisplay::setTransparentFramePadding(bool enabled)
{
    if (m_transparentFramePadding == enabled)
        return;
    m_transparentFramePadding = enabled;
    updateBackgroundAttributes();
    update();
}

void VideoDisplay::setIdleImage(const QPixmap &image)
{
    m_idleImage = image;
    updateBackgroundAttributes();
    update();
}

void VideoDisplay::updateBackgroundAttributes()
{
    const bool hasFrame = m_hasActiveVideo && m_currentFrame;
    const bool opaque = hasFrame ? !m_transparentFramePadding
                                 : !m_transparentIdleBackground && m_idleImage.isNull();
    if (testAttribute(Qt::WA_OpaquePaintEvent) == opaque)
        return;
    setAttribute(Qt::WA_OpaquePaintEvent, opaque);
    setAttribute(Qt::WA_NoSystemBackground, opaque);
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
        if (m_transparentIdleBackground || m_transparentFramePadding || !m_idleImage.isNull())
            updateBackgroundAttributes();
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
    if (m_transparentIdleBackground || m_transparentFramePadding || !m_idleImage.isNull())
        updateBackgroundAttributes();
    update();
}

void VideoDisplay::clearFrame()
{
    m_currentFrame.reset();
    m_hasActiveVideo = false;
    if (m_transparentIdleBackground || m_transparentFramePadding || !m_idleImage.isNull())
        updateBackgroundAttributes();
    update();
}

void VideoDisplay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    if (m_hasActiveVideo && m_currentFrame)
    {
        if (!m_transparentFramePadding)
            painter.fillRect(event->rect(), Qt::black);
        const bool prescale = m_currentFrame->isCdgSized() && m_settings.cdgPrescalingEnabled();
        const auto scaled = m_currentFrame->scaled(size(), prescale);
        int x = (width() - scaled.width()) / 2;
        int y = (height() - scaled.height()) / 2;
        painter.drawImage(x, y, scaled);
    }
    else if (!m_idleImage.isNull())
    {
        const QSize fitted = m_idleImage.size().scaled(size(), Qt::KeepAspectRatio);
        QRect target(QPoint(), fitted);
        target.moveCenter(rect().center());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(target, m_idleImage, m_idleImage.rect());
    }
    else
    {
        if (!m_transparentIdleBackground)
            painter.fillRect(event->rect(), Qt::black);
        if (m_useDefaultBg)
        {
            const auto logo = m_transparentIdleBackground
                ? QStringLiteral(":/icons/Icons/openkj-splash-logo.svg")
                : QStringLiteral(":/icons/Icons/okjlogo.svg");
            QSvgRenderer renderer(logo);
            renderer.setAspectRatioMode(Qt::KeepAspectRatio);
            renderer.render(&painter);
        }
        else if (!m_transparentIdleBackground)
        {
            painter.drawPixmap(rect(), m_currentBg, m_currentBg.rect());
        }
    }
}
