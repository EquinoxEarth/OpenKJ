#include "rotationdisplay.h"
#include "models/tablemodelrotation.h"
#include "videodisplay.h"

#include <QAction>
#include <QBuffer>
#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHideEvent>
#include <QImageReader>
#include <QLabel>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScreen>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QWindow>
#include <QtConcurrentRun>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr qreal tickerPixelsPerMillisecond = 0.075;
constexpr qint64 backgroundVideoCacheLimit = 256 * 1024 * 1024;

qreal tickerCardInset(qreal height)
{
    return qBound(10.0, height * 0.24, 28.0);
}
}

class RotationDisplay::TickerStrip : public QWidget
{
public:
    explicit TickerStrip(RotationDisplay &display) : QWidget(&display), m_display(display)
    {
        setObjectName("rotationTickerStrip");
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void invalidate()
    {
        m_dirty = true;
        update();
    }

    void repaintForDisplayRefresh(bool backgroundChanged)
    {
        m_dirty = true;
        if (backgroundChanged)
            m_display.repaint();
        else
            repaint();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal scale = devicePixelRatioF();
        const QSize pixelSize(qCeil(width() * scale), qCeil(height() * scale));
        if (pixelSize.isEmpty())
            return;
        if (m_frame.size() != pixelSize || m_frame.devicePixelRatio() != scale) {
            m_frame = QPixmap(pixelSize);
            m_frame.setDevicePixelRatio(scale);
            m_dirty = true;
        }
        if (m_dirty) {
            m_frame.fill(Qt::transparent);
            QPainter painter(&m_frame);
            painter.setRenderHint(QPainter::Antialiasing);
            m_display.paintTickerCards(painter, rect());
            m_dirty = false;
        }
        QPainter painter(this);
        painter.drawPixmap(0, 0, m_frame);
    }

private:
    RotationDisplay &m_display;
    QPixmap m_frame;
    bool m_dirty{true};
};

RotationDisplay::RotationDisplay(TableModelRotation &rotation, QWidget *parent)
    : QWidget(parent, Qt::Window), m_rotation(rotation),
      m_syncTickerToDisplay(QGuiApplication::platformName() == QStringLiteral("cocoa"))
{
    setObjectName("RotationDisplay");
    setWindowTitle(tr("OpenKJ — Rotation Display"));
    setAccessibleName(tr("Upcoming singers"));
    setAttribute(Qt::WA_QuitOnClose, false);
    m_videoDisplay = new VideoDisplay(this);
    m_videoDisplay->setObjectName("rotationVideoDisplay");
    m_videoDisplay->setAccessibleName(tr("Karaoke lyrics and video"));
    m_videoDisplay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_videoDisplay->setTransparentFramePadding(true);
    m_summary = new QLabel(this);
    m_summary->setObjectName("rotationSummary");
    m_summary->setTextFormat(Qt::PlainText);
    m_summary->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_summary->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_summary->hide();
    m_tickerStrip = new TickerStrip(*this);
    m_tickerStrip->hide();
    setMinimumSize(320, 240);
    resize(600, 720);
    setToolTip(tr("Double-click or press F11 to toggle fullscreen. Escape leaves fullscreen. Right-click for controls."));

    setContextMenuPolicy(Qt::ActionsContextMenu);
    m_fullscreenAction = new QAction(tr("Fullscreen"), this);
    m_fullscreenAction->setCheckable(true);
    addAction(m_fullscreenAction);
    connect(m_fullscreenAction, &QAction::toggled, this, &RotationDisplay::setFullscreen);
    auto *hideAction = new QAction(tr("Hide Rotation Display"), this);
    addAction(hideAction);
    connect(hideAction, &QAction::triggered, this, [this] { setDisplayVisible(false); });
    auto *fullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fullscreenShortcut, &QShortcut::activated, this, [this] { setFullscreen(!isFullScreen()); });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, [this] {
        if (isFullScreen())
            setFullscreen(false);
    });
    connect(&rotation, &TableModelRotation::rotationModified, this, &RotationDisplay::refresh, Qt::QueuedConnection);
    m_tickerAnimation.setObjectName("rotationTickerAnimation");
    m_tickerAnimation.setStartValue(0.0);
    m_tickerAnimation.setEndValue(1.0);
    m_tickerAnimation.setLoopCount(-1);
    connect(&m_tickerAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &progress) {
        m_tickerOffset = progress.toReal() * m_tickerCycleWidth;
        m_tickerStrip->invalidate();
    });
    m_backgroundFramePool.setMaxThreadCount(1);
    connect(&m_backgroundLoadWatcher, &QFutureWatcher<QByteArray>::finished, this, [this] {
        const auto source = std::exchange(m_loadingBackgroundSource, {});
        auto data = m_backgroundLoadWatcher.future().takeResult();
        if (m_backgroundIsVideo && source == QUrl::fromLocalFile(m_backgroundImagePath)) {
            setBackgroundVideoSource(source, std::move(data));
            if (isVisible())
                m_backgroundPlayer->play();
        } else {
            updateBackgroundPlayback();
        }
    });
    connect(&m_backgroundFrameWatcher, &QFutureWatcher<QImage>::finished, this, [this] {
        auto image = m_backgroundFrameWatcher.future().takeResult();
        m_backgroundFramePreparing = false;
        if (!image.isNull() && isVisible() && m_backgroundIsVideo &&
            m_preparingBackgroundGeneration == m_backgroundGeneration) {
            m_readyBackgroundFrame = QPixmap::fromImage(std::move(image));
            if (!m_tickerRefreshClock.isValid() && presentBackgroundFrame())
                update();
        }
        prepareBackgroundFrame();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] {
        QTimer::singleShot(0, this, &RotationDisplay::ensureOnScreen);
    });
    restoreGeometry(m_settings.rotationDisplayGeometry());
    rememberWindowedGeometry();
    applySettings();
}

QString RotationDisplay::currentSingerName() const
{
    return displayName(m_currentSinger);
}

QString RotationDisplay::displayName(const QString &name) const
{
    if (!m_hideNameNotes)
        return name;
    QString displayed;
    displayed.reserve(name.size());
    int depth = 0;
    for (const auto character : name) {
        if (character == QLatin1Char('('))
            ++depth;
        else if (character == QLatin1Char(')') && depth > 0)
            --depth;
        else if (depth == 0)
            displayed += character;
    }
    return displayed.simplified();
}

void RotationDisplay::setCurrentSinger(const QString &name, bool singing)
{
    const auto current = singing ? name.trimmed() : QString{};
    if (current == m_currentSinger)
        return;
    m_currentSinger = current;
    refresh();
}

void RotationDisplay::refresh()
{
    const int limit = m_ticker ? std::numeric_limits<int>::max() : m_settings.rotationDisplaySingerCount();
    auto names = m_rotation.upcomingSingerNames(limit, m_settings.rotationDisplayShowEmptySingers());
    for (auto &name : names)
        name = displayName(name);
    if (m_names != names) {
        m_names = std::move(names);
        rebuildTickerCards();
    }
    const auto upcoming = m_names.isEmpty() ? tr("Waiting for singers") : tr("Up next: %1").arg(m_names.join(", "));
    const auto current = currentSingerName();
    setAccessibleDescription(current.isEmpty() ? upcoming
        : tr("Now singing: %1. %2").arg(current, upcoming));
    if (!m_summary->isHidden())
        updateSummary(m_rotation.rotationDuration());
    update();
}

void RotationDisplay::updateSummary(int durationSeconds)
{
    if (m_summary->isHidden())
        return;

    const int minutes = qCeil(qMax(0, durationSeconds) / 60.0);
    const auto duration = minutes < 60 ? tr("%L1 min").arg(minutes)
        : tr("%L1 hr %L2 min").arg(minutes / 60).arg(minutes % 60);
    const auto text = tr("Singers: %L1  |  Est. rotation: %2").arg(m_rotation.singerCount()).arg(duration);
    if (m_summary->text() != text) {
        m_summary->setText(text);
        updateContentGeometry();
        update();
    }
}

void RotationDisplay::applySettings()
{
    setFont(m_settings.rotationDisplayFont());
    m_ticker = m_settings.rotationDisplayTicker();
    m_textColor = m_settings.rotationDisplayTextColor();
    const qreal opacity = m_settings.rotationDisplayRowOpacity() / 100.0;
    m_rowAlpha = qRound(255 * opacity);
    // Keep the first row highlighted while allowing fully transparent or opaque rows.
    m_firstRowAlpha = qRound(255 * (1 - qPow(1 - opacity, 2.4)));
    m_accentColor = m_settings.rotationDisplayAccentColor();
    m_backgroundColor = m_settings.rotationDisplayBackgroundColor();
    m_backgroundDimAlpha = qRound(m_settings.rotationDisplayBackgroundDimming() * 255.0 / 100);
    const auto backgroundPath = m_settings.rotationDisplayBackgroundImage();
    if (backgroundPath != m_backgroundImagePath ||
        (!m_backgroundIsVideo && !backgroundPath.isEmpty() && m_backgroundImage.isNull())) {
        discardPendingBackgroundFrames();
        m_backgroundImagePath = backgroundPath;
        m_backgroundIsVideo = VideoDisplay::isBackgroundVideo(backgroundPath);
        m_backgroundImage = {};
        if (!m_backgroundIsVideo && !backgroundPath.isEmpty()) {
            QImageReader reader(backgroundPath);
            reader.setAutoTransform(true);
            m_backgroundImage = QPixmap::fromImage(reader.read());
        }
        updateBackgroundPlayback();
    }
    m_videoDisplay->setTransparentIdleBackground(m_ticker || !m_backgroundImage.isNull());
    QImageReader idleImage(m_settings.rotationDisplayIdleImage());
    idleImage.setAutoTransform(true);
    m_videoDisplay->setIdleImage(idleImage.fileName().isEmpty() ? QPixmap{} : QPixmap::fromImage(idleImage.read()));
    m_showHeading = m_settings.rotationDisplayShowHeading();
    m_hideNameNotes = m_settings.rotationDisplayHideNameNotes();
    m_videoDisplay->setVisible(m_settings.rotationDisplayVideoEnabled());
    auto summaryPalette = m_summary->palette();
    summaryPalette.setColor(QPalette::WindowText, m_textColor);
    m_summary->setPalette(summaryPalette);
    m_summary->setVisible(m_settings.rotationDisplayShowSummary());
    refresh();
    updateContentGeometry();
}

void RotationDisplay::updateBackgroundPlayback()
{
    const auto source = m_backgroundIsVideo
        ? QUrl::fromLocalFile(m_backgroundImagePath) : QUrl{};
    if (!m_backgroundPlayer) {
        if (!isVisible() || source.isEmpty())
            return;
        m_backgroundPlayer = new QMediaPlayer(this);
        auto *sink = new QVideoSink(m_backgroundPlayer);
        m_backgroundPlayer->setVideoSink(sink);
        m_backgroundPlayer->setLoops(QMediaPlayer::Infinite);
        connect(m_backgroundPlayer, &QMediaPlayer::tracksChanged, this, [this] {
            m_backgroundPlayer->setActiveAudioTrack(-1);
        });
        connect(m_backgroundPlayer, &QMediaPlayer::errorOccurred, this, [this] {
            if (!m_backgroundBuffer || m_backgroundPlayer->error() == QMediaPlayer::NoError)
                return;
            const auto source = m_backgroundPlayer->source();
            setBackgroundVideoSource(source);
            if (isVisible())
                m_backgroundPlayer->play();
        }, Qt::QueuedConnection);
        connect(sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
            if (!isVisible() || !m_backgroundIsVideo || !frame.isValid())
                return;
            m_pendingBackgroundFrame = frame;
            prepareBackgroundFrame();
        });
    }
    if (m_backgroundPlayer->source() != source) {
        if (!m_backgroundPlayer->source().isEmpty())
            setBackgroundVideoSource({});
        if (isVisible() && !source.isEmpty() && m_loadingBackgroundSource.isEmpty()) {
            m_loadingBackgroundSource = source;
            m_backgroundLoadWatcher.setFuture(QtConcurrent::run([path = source.toLocalFile()] {
                QFile file(path);
                if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly))
                    return QByteArray{};
                const auto size = file.size();
                if (size <= 0 || size > backgroundVideoCacheLimit)
                    return QByteArray{};
                auto data = file.read(size + 1);
                return data.size() == size ? data : QByteArray{};
            }));
        }
        return;
    }
    if (source.isEmpty())
        return;
    if (isVisible()) {
        m_backgroundPlayer->play();
    } else {
        discardPendingBackgroundFrames();
        m_backgroundPlayer->pause();
    }
}

void RotationDisplay::setBackgroundVideoSource(const QUrl &source, QByteArray data)
{
    discardPendingBackgroundFrames();
    auto *oldBuffer = std::exchange(m_backgroundBuffer, nullptr);
    if (data.isEmpty()) {
        m_backgroundPlayer->setSource(source);
    } else {
        m_backgroundBuffer = new QBuffer(m_backgroundPlayer);
        m_backgroundBuffer->setData(std::move(data));
        m_backgroundBuffer->open(QIODevice::ReadOnly);
        m_backgroundPlayer->setSourceDevice(m_backgroundBuffer, source);
    }
    if (oldBuffer)
        oldBuffer->deleteLater();
}

void RotationDisplay::discardPendingBackgroundFrames()
{
    ++m_backgroundGeneration;
    m_pendingBackgroundFrame = {};
    m_readyBackgroundFrame = {};
}

void RotationDisplay::prepareBackgroundFrame()
{
    if (m_backgroundFramePreparing || !m_pendingBackgroundFrame.isValid())
        return;
    m_backgroundFramePreparing = true;
    const auto frame = std::exchange(m_pendingBackgroundFrame, {});
    m_preparingBackgroundGeneration = m_backgroundGeneration;
    const qreal scale = devicePixelRatioF();
    const QSize pixels(qCeil(width() * scale), qCeil(height() * scale));
    m_backgroundFrameWatcher.setFuture(QtConcurrent::run(&m_backgroundFramePool,
        [frame, pixels, scale, color = m_backgroundColor, dim = m_backgroundDimAlpha] {
        const auto source = frame.toImage();
        if (source.isNull())
            return QImage{};
        QImage result(pixels, QImage::Format_RGB32);
        if (result.isNull())
            return result;
        if (source.hasAlphaChannel())
            result.fill(color.rgb());
        {
            QPainter painter(&result);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            QRect target(QPoint(), source.size().scaled(pixels, Qt::KeepAspectRatioByExpanding));
            target.moveCenter(result.rect().center());
            painter.drawImage(target, source);
            if (dim > 0)
                painter.fillRect(result.rect(), QColor(0, 0, 0, dim));
        }
        result.setDevicePixelRatio(scale);
        return result;
    }));
}

bool RotationDisplay::presentBackgroundFrame()
{
    if (m_readyBackgroundFrame.isNull())
        return false;
    m_backgroundImage = std::exchange(m_readyBackgroundFrame, {});
    m_videoDisplay->setTransparentIdleBackground(true);
    return true;
}

void RotationDisplay::updateContentGeometry()
{
    const qreal margin = qBound(12.0, m_ticker ? qMin(width() * 0.055, height() * 0.065) : width() * 0.055, 64.0);
    m_singerArea = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
    if (m_summary && !m_summary->isHidden()) {
        QFont summaryFont = font();
        summaryFont.setPixelSize(qBound(12, qRound(height() * 0.027), 32));
        const qreal textWidth = QFontMetricsF(summaryFont).horizontalAdvance(m_summary->text());
        if (textWidth > m_singerArea.width())
            summaryFont.setPixelSize(qMax(8, qFloor(summaryFont.pixelSize() * m_singerArea.width() / textWidth)));
        m_summary->setFont(summaryFont);
        const int summaryHeight = QFontMetrics(summaryFont).height();
        const qreal top = m_singerArea.bottom() - summaryHeight;
        m_summary->setGeometry(QRectF(m_singerArea.left(), top, m_singerArea.width(), summaryHeight).toAlignedRect());
        m_singerArea.setBottom(top - margin * 0.4);
    }
    const qreal gap = margin * 0.75;
    if (m_ticker) {
        QRectF videoArea = m_singerArea;
        const qreal labelHeight = qBound(18.0, height() * 0.04, 48.0);
        m_tickerCurrentArea = QRectF(videoArea.left(), videoArea.top(), videoArea.width(), labelHeight);
        const qreal cardHeight = qBound(30.0, height() * 0.105, 96.0);
        m_singerArea.setTop(m_singerArea.bottom() - cardHeight);
        const qreal headingHeight = m_showHeading ? labelHeight * 0.75 : 0;
        m_tickerHeadingArea = QRectF(m_singerArea.left(), m_singerArea.top() - headingHeight - gap * 0.3,
                                     m_singerArea.width(), headingHeight);
        videoArea.setTop(m_tickerCurrentArea.bottom() + gap * 0.4);
        videoArea.setBottom(m_tickerHeadingArea.top() - gap * 0.55);
        videoArea.adjust(gap, 0, -gap, 0);
        if (m_videoDisplay && !m_videoDisplay->isHidden())
            m_videoDisplay->setGeometry(videoArea.toAlignedRect());
        m_tickerStrip->setGeometry(m_singerArea.toAlignedRect());
        m_tickerStrip->show();
        rebuildTickerCards();
        return;
    }
    m_tickerStrip->hide();
    rebuildTickerCards();
    if (!m_videoDisplay || m_videoDisplay->isHidden())
        return;
    const qreal videoWidth = (m_singerArea.width() - gap) * 0.7;
    QRectF videoArea = m_singerArea;
    videoArea.setWidth(videoWidth);
    if (m_settings.rotationDisplayVideoOnLeft()) {
        m_singerArea.setLeft(videoArea.right() + gap);
    } else {
        videoArea.moveRight(m_singerArea.right());
        m_singerArea.setRight(videoArea.left() - gap);
    }
    m_videoDisplay->setGeometry(videoArea.toAlignedRect());
}

int RotationDisplay::maxFontPixels() const
{
    const auto displayFont = font();
    return displayFont.pixelSize() > 0 ? displayFont.pixelSize()
        : qMax(8, qRound(displayFont.pointSizeF() * logicalDpiY() / 72.0));
}

void RotationDisplay::rebuildTickerCards()
{
    m_tickerCards.clear();
    m_tickerCycleWidth = 0;
    if (m_ticker && !m_tickerStrip->rect().isEmpty() && !m_names.isEmpty()) {
        const qreal height = m_tickerStrip->height();
        const qreal gap = qMin(12.0, height * 0.12);
        const qreal inset = tickerCardInset(height);
        const qreal maxWidth = m_tickerStrip->width() - gap;
        m_tickerFont = font();
        m_tickerFont.setBold(false);
        m_tickerFont.setPixelSize(qMin(maxFontPixels(), qMax(12, qRound(height * 0.46))));
        m_tickerFirstFont = m_tickerFont;
        m_tickerFirstFont.setBold(true);
        const QFontMetricsF normalMetrics(m_tickerFont);
        const QFontMetricsF boldMetrics(m_tickerFirstFont);
        m_tickerNumberWidth = boldMetrics.horizontalAdvance(QString::number(m_names.size())) + inset;
        const qreal fixedWidth = 2 * inset + m_tickerNumberWidth;
        const qreal nameWidth = qMax(1.0, maxWidth - fixedWidth);
        m_tickerCards.reserve(m_names.size());
        for (qsizetype row = 0; row < m_names.size(); ++row) {
            const auto &metrics = row == 0 ? boldMetrics : normalMetrics;
            const qreal width = qMin(maxWidth, qMax(height * 2, fixedWidth + metrics.horizontalAdvance(m_names[row])));
            m_tickerCards.append({m_tickerCycleWidth, width, QString::number(row + 1),
                                  metrics.elidedText(m_names[row], Qt::ElideRight, nameWidth)});
            m_tickerCycleWidth += width + gap;
        }
        if (m_syncTickerToDisplay) {
            m_tickerOffset = std::fmod(m_tickerOffset, m_tickerCycleWidth);
        } else {
            const int duration = qRound(qMin(m_tickerCycleWidth / tickerPixelsPerMillisecond,
                                         qreal(std::numeric_limits<int>::max())));
            if (m_tickerAnimation.duration() != duration || m_tickerOffset >= m_tickerCycleWidth) {
                const QSignalBlocker blocker(m_tickerAnimation);
                const qreal offset = std::fmod(m_tickerOffset, m_tickerCycleWidth);
                m_tickerAnimation.setDuration(duration);
                m_tickerAnimation.setCurrentTime(qMin(duration - 1, qFloor(offset * duration / m_tickerCycleWidth)));
                m_tickerOffset = m_tickerAnimation.currentValue().toReal() * m_tickerCycleWidth;
            }
        }
    }
    m_tickerStrip->invalidate();
    updateTickerAnimation();
}

void RotationDisplay::updateTickerAnimation()
{
    if (m_syncTickerToDisplay) {
        auto *window = windowHandle();
        if (m_tickerWindow != window) {
            if (m_tickerWindow)
                m_tickerWindow->removeEventFilter(this);
            m_tickerWindow = window;
            if (window)
                window->installEventFilter(this);
            m_tickerRefreshClock.invalidate();
        }
        if (!m_ticker || m_tickerCards.isEmpty())
            m_tickerOffset = 0;
        if (m_ticker && !m_tickerCards.isEmpty() && isVisible() && !isMinimized() && window && window->isExposed()) {
            if (!m_tickerRefreshClock.isValid()) {
                m_tickerRefreshClock.start();
                window->requestUpdate();
            }
        } else {
            m_tickerRefreshClock.invalidate();
            if (isVisible() && m_backgroundIsVideo && presentBackgroundFrame())
                update();
        }
        return;
    }

    const auto state = m_tickerAnimation.state();
    if (!m_ticker || m_tickerCards.isEmpty()) {
        m_tickerAnimation.stop();
        m_tickerOffset = 0;
    } else if (!isVisible() || isMinimized()) {
        if (state == QAbstractAnimation::Running)
            m_tickerAnimation.pause();
    } else if (state == QAbstractAnimation::Stopped) {
        m_tickerAnimation.start();
    } else if (state == QAbstractAnimation::Paused) {
        m_tickerAnimation.resume();
    }
}

bool RotationDisplay::eventFilter(QObject *watched, QEvent *event)
{
    if (m_tickerWindow && watched == m_tickerWindow) {
        if (event->type() == QEvent::Expose) {
            updateTickerAnimation();
        } else if (event->type() == QEvent::UpdateRequest && m_tickerRefreshClock.isValid()) {
            const qreal elapsedMs = m_tickerRefreshClock.nsecsElapsed() / 1000000.0;
            m_tickerRefreshClock.start();
            m_tickerOffset = std::fmod(m_tickerOffset + elapsedMs * tickerPixelsPerMillisecond, m_tickerCycleWidth);
            const bool backgroundChanged = presentBackgroundFrame();
            m_tickerStrip->repaintForDisplayRefresh(backgroundChanged);
            if (m_tickerRefreshClock.isValid() && m_tickerWindow)
                m_tickerWindow->requestUpdate();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void RotationDisplay::drawSingerCard(QPainter &painter, const QRectF &rect, qreal radius, qreal inset,
                                     qreal numberWidth, const QString &number, const QString &name, bool first) const
{
    QColor fill = m_textColor;
    fill.setAlpha(first ? m_firstRowAlpha : m_rowAlpha);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, radius, radius);
    painter.setPen(first ? m_accentColor : m_textColor);
    painter.drawText(rect.adjusted(inset, 0, -inset, 0), Qt::AlignLeft | Qt::AlignVCenter, number);
    painter.setPen(m_textColor);
    painter.drawText(rect.adjusted(inset + numberWidth, 0, -inset, 0),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, name);
}

void RotationDisplay::paintTickerHeadings(QPainter &painter, const QPaintEvent &event) const
{
    QFont headingFont = font();
    headingFont.setBold(true);
    if (!m_currentSinger.isEmpty() && event.region().intersects(m_tickerCurrentArea.toAlignedRect())) {
        headingFont.setPixelSize(qMax(8, qRound(m_tickerCurrentArea.height() * 0.68)));
        painter.setFont(headingFont);
        const QFontMetricsF metrics(headingFont);
        const auto heading = tr("NOW SINGING");
        const qreal headingWidth = metrics.horizontalAdvance(heading);
        const qreal gap = metrics.horizontalAdvance(u' ') * 2;
        const auto name = metrics.elidedText(currentSingerName(), Qt::ElideRight,
                                            qMax(0.0, m_tickerCurrentArea.width() - headingWidth - gap));
        const qreal nameWidth = metrics.horizontalAdvance(name);
        const qreal start = m_tickerCurrentArea.center().x() - (headingWidth + gap + nameWidth) / 2;
        QRectF line(start, m_tickerCurrentArea.top(), headingWidth + gap, m_tickerCurrentArea.height());
        painter.setPen(m_textColor);
        painter.drawText(line, Qt::AlignLeft | Qt::AlignVCenter, heading);
        line.moveLeft(line.right());
        line.setWidth(nameWidth + gap);
        painter.setPen(m_accentColor);
        painter.drawText(line, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, name);
    }
    if (m_showHeading && event.region().intersects(m_tickerHeadingArea.toAlignedRect())) {
        headingFont.setPixelSize(qMax(8, qRound(m_tickerHeadingArea.height() * 0.74)));
        painter.setFont(headingFont);
        painter.setPen(m_textColor);
        painter.drawText(m_tickerHeadingArea, Qt::AlignLeft | Qt::AlignVCenter, tr("UP NEXT"));
    }
}

void RotationDisplay::paintTickerCards(QPainter &painter, const QRectF &area) const
{
    if (m_tickerCards.isEmpty()) {
        QFont waitingFont = font();
        waitingFont.setBold(true);
        waitingFont.setPixelSize(qMin(maxFontPixels(), qMax(12, qRound(area.height() * 0.35))));
        painter.setFont(waitingFont);
        painter.setPen(m_textColor);
        painter.drawText(area, Qt::AlignCenter, tr("Waiting for singers"));
        return;
    }
    const qreal height = area.height();
    const qreal gap = qMin(12.0, height * 0.12);
    const qreal inset = tickerCardInset(height);
    const qreal radius = qMin(12.0, height * 0.15);
    const auto firstVisible = std::upper_bound(m_tickerCards.cbegin(), m_tickerCards.cend(), m_tickerOffset,
        [](qreal offset, const TickerCard &card) { return offset < card.offset; }) - 1;
    auto index = firstVisible - m_tickerCards.cbegin();
    qreal x = area.left() + firstVisible->offset - m_tickerOffset;
    // Reuse the same spacing at the cycle boundary, including for a single singer.
    while (x < area.right()) {
        const auto &card = m_tickerCards[index];
        painter.setFont(index == 0 ? m_tickerFirstFont : m_tickerFont);
        drawSingerCard(painter, QRectF(x, area.top(), card.width, height), radius,
                       inset, m_tickerNumberWidth, card.number, card.name, index == 0);
        x += card.width + gap;
        index = (index + 1) % m_tickerCards.size();
    }
}

void RotationDisplay::saveWindowedGeometry()
{
    rememberWindowedGeometry();
    m_settings.setRotationDisplayGeometry(m_windowedGeometry);
}

void RotationDisplay::rememberWindowedGeometry()
{
    if (!m_changingState && !isFullScreen() && !isMaximized() && !isMinimized() &&
        (!m_fullscreenAction || !m_fullscreenAction->isChecked()))
        m_windowedGeometry = saveGeometry();
}

void RotationDisplay::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    rememberWindowedGeometry();
}

void RotationDisplay::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    rememberWindowedGeometry();
    updateContentGeometry();
}

void RotationDisplay::setDisplayVisible(bool visible)
{
    if (!visible) {
        hide();
        return;
    }
    refresh();
    ensureOnScreen();
    if (m_settings.rotationDisplayFullscreen())
        showFullScreen();
    else
        showNormal();
    raise();
}

void RotationDisplay::setFullscreen(bool fullscreen)
{
    if (fullscreen == isFullScreen())
        return;
    saveWindowedGeometry();
    const QScopedValueRollback changingState(m_changingState, true);
    m_settings.setRotationDisplayFullscreen(fullscreen);
    if (fullscreen)
        showFullScreen();
    else {
        showNormal();
        restoreGeometry(m_windowedGeometry);
        ensureOnScreen();
    }
    m_settings.setRotationDisplayVisible(isVisible());
    emit visibilityChanged(isVisible());
}

void RotationDisplay::ensureOnScreen()
{
    const auto screens = QGuiApplication::screens();
    if (std::any_of(screens.cbegin(), screens.cend(), [this](QScreen *screen) {
        return screen->availableGeometry().intersects(frameGeometry());
    }))
        return;
    if (auto *screen = QGuiApplication::primaryScreen()) {
        setScreen(screen);
        if (!isFullScreen()) {
            resize(size().boundedTo(screen->availableGeometry().size()));
            move(screen->availableGeometry().center() - rect().center());
        }
    }
}

void RotationDisplay::saveForShutdown()
{
    m_settings.setRotationDisplayVisible(isVisible());
    saveWindowedGeometry();
    m_shuttingDown = true;
    hide();
}

void RotationDisplay::closeEvent(QCloseEvent *event)
{
    setDisplayVisible(false);
    event->ignore();
}

void RotationDisplay::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateTickerAnimation();
    updateBackgroundPlayback();
    if (!m_shuttingDown && !m_changingState)
        m_settings.setRotationDisplayVisible(true);
    if (!m_changingState)
        emit visibilityChanged(true);
}

void RotationDisplay::hideEvent(QHideEvent *event)
{
    if (!m_changingState)
        saveWindowedGeometry();
    if (!m_shuttingDown && !m_changingState)
        m_settings.setRotationDisplayVisible(false);
    QWidget::hideEvent(event);
    updateTickerAnimation();
    updateBackgroundPlayback();
    if (!m_changingState)
        emit visibilityChanged(false);
}

void RotationDisplay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFullscreen(!isFullScreen());
        event->accept();
    } else {
        QWidget::mouseDoubleClickEvent(event);
    }
}

void RotationDisplay::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        const QSignalBlocker blocker(m_fullscreenAction);
        m_fullscreenAction->setChecked(isFullScreen());
        m_settings.setRotationDisplayFullscreen(isFullScreen());
        updateTickerAnimation();
    }
}

void RotationDisplay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_backgroundIsVideo && !m_backgroundImage.isNull()) {
        if (m_backgroundImage.deviceIndependentSize() == QSizeF(size()))
            painter.drawPixmap(0, 0, m_backgroundImage);
        else
            painter.drawPixmap(rect(), m_backgroundImage);
    } else {
        painter.fillRect(rect(), m_backgroundColor);
    }
    if (!m_backgroundIsVideo && !m_backgroundImage.isNull()) {
        const QSize imageSize = m_backgroundImage.size().scaled(size(), Qt::KeepAspectRatioByExpanding);
        QRectF target(QPointF(0, 0), QSizeF(imageSize));
        target.moveCenter(QRectF(rect()).center());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(target, m_backgroundImage, m_backgroundImage.rect());
        if (m_backgroundDimAlpha > 0)
            painter.fillRect(rect(), QColor(0, 0, 0, m_backgroundDimAlpha));
    }
    if (m_ticker) {
        paintTickerHeadings(painter, *event);
        return;
    }
    if (!event->region().intersects(m_singerArea.toAlignedRect()))
        return;
    const qreal margin = qBound(12.0, width() * 0.055, 64.0);
    QRectF content = m_singerArea;
    QFont displayFont = font();
    const int maxPixels = maxFontPixels();
    QFont currentHeadingFont = displayFont;
    currentHeadingFont.setPixelSize(qBound(8, qRound(content.height() * 0.035), 28));
    currentHeadingFont.setBold(true);
    const auto currentHeading = tr("NOW SINGING");
    const auto currentHeadingWidth = QFontMetricsF(currentHeadingFont).horizontalAdvance(currentHeading);
    if (currentHeadingWidth > content.width())
        currentHeadingFont.setPixelSize(qMax(6, qFloor(currentHeadingFont.pixelSize() * content.width() / currentHeadingWidth)));
    painter.setFont(currentHeadingFont);
    painter.setPen(m_textColor);
    const qreal currentHeadingHeight = QFontMetricsF(currentHeadingFont).height();
    painter.drawText(QRectF(content.left(), content.top(), content.width(), currentHeadingHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, currentHeading);
    content.setTop(content.top() + currentHeadingHeight + margin * 0.2);
    QFont currentFont = displayFont;
    currentFont.setBold(true);
    currentFont.setPixelSize(qMin(maxPixels, qMax(8, qRound(m_singerArea.height() * 0.075))));
    const auto currentWidth = QFontMetricsF(currentFont).horizontalAdvance(currentSingerName());
    if (currentWidth > content.width())
        currentFont.setPixelSize(qMax(8, qFloor(currentFont.pixelSize() * content.width() / currentWidth)));
    painter.setFont(currentFont);
    painter.setPen(m_currentSinger.isEmpty() ? m_textColor : m_accentColor);
    const qreal currentHeight = QFontMetricsF(currentFont).height();
    const auto currentText = QFontMetrics(currentFont).elidedText(currentSingerName(), Qt::ElideRight,
                                                               qMax(0, qFloor(content.width())));
    painter.drawText(QRectF(content.left(), content.top(), content.width(), currentHeight),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, currentText);
    content.setTop(content.top() + currentHeight + margin * 0.65);
    QColor separator = m_textColor;
    separator.setAlpha(40);
    painter.setPen(separator);
    painter.drawLine(QPointF(content.left(), content.top()), QPointF(content.right(), content.top()));
    content.setTop(content.top() + margin * 0.65);
    if (m_showHeading) {
        QFont headingFont = displayFont;
        headingFont.setPixelSize(qBound(12, qRound(height() * 0.035), 36));
        headingFont.setBold(true);
        const auto headingWidth = QFontMetricsF(headingFont).horizontalAdvance(tr("UP NEXT"));
        if (headingWidth > content.width())
            headingFont.setPixelSize(qMax(8, qFloor(headingFont.pixelSize() * content.width() / headingWidth)));
        painter.setFont(headingFont);
        painter.setPen(m_textColor);
        const qreal headingHeight = QFontMetricsF(headingFont).height();
        painter.drawText(QRectF(content.left(), content.top(), content.width(), headingHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, tr("UP NEXT"));
        content.setTop(content.top() + headingHeight + margin * 0.65);
    }
    if (m_names.isEmpty()) {
        displayFont.setPixelSize(qMin(maxPixels, qMax(12, qRound(content.width() / 24))));
        painter.setFont(displayFont);
        painter.setPen(m_textColor);
        painter.drawText(content, Qt::AlignCenter | Qt::TextWordWrap, tr("Waiting for singers"));
        return;
    }
    constexpr qreal maxRowHeight = 96.0;
    const qreal rowHeight = qMin(maxRowHeight, content.height() / m_names.size());
    const qreal gap = qMin(12.0, rowHeight * 0.12);
    displayFont.setPixelSize(qMin(maxPixels, qMax(4, qRound((rowHeight - gap) * 0.46))));
    // Keep the same font size across rows and fit names into a narrow side panel.
    QFont boldFont = displayFont;
    boldFont.setBold(true);
    const QFontMetricsF metrics(boldFont);
    qreal textWidth = 0;
    for (const auto &name : m_names)
        textWidth = qMax(textWidth, metrics.horizontalAdvance(name));
    const qreal inset = qMin(24.0, rowHeight * 0.15);
    const qreal availableWidth = qMax(1.0, content.width() - inset * 3);
    textWidth += metrics.horizontalAdvance(QString::number(m_names.size()));
    if (textWidth > availableWidth)
        displayFont.setPixelSize(qMax(qMax(4, displayFont.pixelSize() / 2),
                                     qFloor(displayFont.pixelSize() * availableWidth / textWidth)));
    for (int row = 0; row < m_names.size(); ++row) {
        const QRectF rowRect(content.left(), content.top() + row * rowHeight,
                             content.width(), rowHeight - gap);
        QFont rowFont = displayFont;
        rowFont.setBold(row == 0);
        painter.setFont(rowFont);
        const qreal numberWidth = QFontMetricsF(rowFont).horizontalAdvance(QString::number(m_names.size())) + inset;
        const auto nameRect = rowRect.adjusted(inset + numberWidth, 0, -inset, 0);
        const auto name = QFontMetrics(rowFont).elidedText(m_names[row], Qt::ElideRight,
                                                         qMax(0, qFloor(nameRect.width())));
        drawSingerCard(painter, rowRect, qMin(12.0, rowHeight * 0.15), inset, numberWidth,
                       QString::number(row + 1), name, row == 0);
    }
}
