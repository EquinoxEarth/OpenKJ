#include "startupsplash.h"
#include "okjversion.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QSvgRenderer>
#include <QtMath>
#include <algorithm>

StartupSplash::StartupSplash()
    : QSplashScreen(QPixmap(), Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAccessibleName(tr("Starting OpenKJ"));

    QFont statusFont = font();
    statusFont.setPointSizeF(std::max(13.0, statusFont.pointSizeF() + 1.0));
    statusFont.setWeight(QFont::DemiBold);
    QFont detailFont = font();
    detailFont.setPointSizeF(std::max(11.0, detailFont.pointSizeF() - 1.0));
    QFont versionFont = detailFont;
    versionFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.4);

    const QString status = tr("Initializing media support");
    const QString hint = tr("First launch may take a little longer.");
    setAccessibleDescription(status + QStringLiteral(". ") + hint);
    const QString version = QStringLiteral("%1  /  %2")
                                .arg(QStringLiteral(OKJ_VERSION_STRING),
                                     QStringLiteral(OKJ_VERSION_BRANCH).toUpper());
    const QFontMetrics statusMetrics(statusFont);
    const QFontMetrics detailMetrics(detailFont);
    const QFontMetrics versionMetrics(versionFont);
    const qreal cardWidth = std::max({480, statusMetrics.horizontalAdvance(status) + 64,
                                     detailMetrics.horizontalAdvance(hint) + 64,
                                     versionMetrics.horizontalAdvance(version) + 64});
    constexpr qreal outerMargin = 10;
    constexpr qreal padding = 32;

    QSvgRenderer logo(QStringLiteral(":/icons/Icons/openkj-splash-logo.svg"));
    const QSizeF logoSize = QSizeF(logo.defaultSize().scaled(286, 120, Qt::KeepAspectRatio));
    qreal y = outerMargin + 34;
    const QRectF logoRect(outerMargin + (cardWidth - logoSize.width()) / 2,
                          y, logoSize.width(), logoSize.height());
    y += logoSize.height() + 18;
    const QRectF versionRect(outerMargin + padding, y, cardWidth - padding * 2, versionMetrics.height());
    y += versionRect.height() + 28;
    const QRectF statusRect(outerMargin + padding, y, cardWidth - padding * 2, statusMetrics.height());
    y += statusRect.height() + 20;
    m_progressTrack = QRectF(outerMargin + 64, y, cardWidth - 128, 4);
    y += m_progressTrack.height() + 18;
    const QRectF hintRect(outerMargin + padding, y, cardWidth - padding * 2, detailMetrics.height());
    const qreal cardHeight = hintRect.bottom() + padding - outerMargin;
    const QRectF card(outerMargin, outerMargin, cardWidth, cardHeight);
    const QSize imageSize(qCeil(cardWidth + outerMargin * 2), qCeil(cardHeight + outerMargin * 2));

    QPixmap image(imageSize * devicePixelRatioF());
    image.setDevicePixelRatio(devicePixelRatioF());
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        for (int spread = 8; spread > 0; --spread) {
            painter.setBrush(QColor(0, 0, 0, 3));
            painter.drawRoundedRect(card.adjusted(-spread, -spread / 2.0, spread, spread),
                                    20 + spread, 20 + spread);
        }

        QLinearGradient background(card.topLeft(), card.bottomRight());
        background.setColorAt(0, QColor("#171f25"));
        background.setColorAt(1, QColor("#101919"));
        painter.setBrush(background);
        painter.drawRoundedRect(card, 20, 20);

        QPainterPath cardClip;
        cardClip.addRoundedRect(card, 20, 20);
        painter.save();
        painter.setClipPath(cardClip);
        QRadialGradient glow(card.topRight(), cardWidth * 0.85);
        glow.setColorAt(0, QColor(62, 210, 128, 30));
        glow.setColorAt(1, Qt::transparent);
        painter.fillRect(card, glow);
        painter.restore();

        painter.setPen(QColor(255, 255, 255, 22));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), 20, 20);
        logo.render(&painter, logoRect);
        painter.setFont(versionFont);
        painter.setPen(QColor("#91a99d"));
        painter.drawText(versionRect, Qt::AlignCenter, version);
        painter.setFont(statusFont);
        painter.setPen(QColor("#eef5f1"));
        painter.drawText(statusRect, Qt::AlignCenter, status);
        painter.setFont(detailFont);
        painter.setPen(QColor("#98a8a3"));
        painter.drawText(hintRect, Qt::AlignCenter, hint);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 20));
        painter.drawRoundedRect(m_progressTrack, 2, 2);
    }
    setPixmap(image);

    m_visibleTime.start();
    m_animationTimer.setInterval(16);
    m_animationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_animationTimer, &QTimer::timeout, this, [this] {
        update(m_progressTrack.toAlignedRect().adjusted(-1, -1, 1, 1));
    });
    m_animationTimer.start();
}

void StartupSplash::drawContents(QPainter *painter)
{
    painter->setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(m_progressTrack, 2, 2);
    painter->setClipPath(clip);
    const qreal phase = ((m_visibleTime.elapsed() + 500) % 1800) / 1800.0;
    const qreal segmentWidth = m_progressTrack.width() * 0.3;
    const qreal left = m_progressTrack.left() - segmentWidth + phase * (m_progressTrack.width() + segmentWidth);
    QLinearGradient highlight(left, 0, left + segmentWidth, 0);
    highlight.setColorAt(0, QColor(62, 210, 128, 0));
    highlight.setColorAt(0.5, QColor("#45d68d"));
    highlight.setColorAt(1, QColor(62, 210, 128, 0));
    painter->fillRect(QRectF(left, m_progressTrack.top(), segmentWidth, m_progressTrack.height()), highlight);
}

void StartupSplash::mousePressEvent(QMouseEvent *event)
{
    // Keep the startup feedback visible until the main window is ready.
    event->ignore();
}

void StartupSplash::finishWhenReady(QWidget *mainWindow)
{
    // Let the main window lay out behind the splash without covering it.
    mainWindow->show();
    raise();

    constexpr int minimumVisibleMs = 3000;
    const int remaining = static_cast<int>(std::max<qint64>(0, minimumVisibleMs - m_visibleTime.elapsed()));
    const auto dismissSplash = [this, mainWindow] {
        m_animationTimer.stop();
        finish(mainWindow);
    };
    if (remaining == 0)
        dismissSplash();
    else
        QTimer::singleShot(remaining, Qt::PreciseTimer, mainWindow, dismissSplash);
}
