#include "rotationdisplay.h"
#include "models/tablemodelrotation.h"
#include "videodisplay.h"

#include <QAction>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QtMath>
#include <algorithm>

RotationDisplay::RotationDisplay(TableModelRotation &rotation, QWidget *parent)
    : QWidget(parent, Qt::Window), m_rotation(rotation)
{
    setObjectName("RotationDisplay");
    setWindowTitle(tr("OpenKJ — Rotation Display"));
    setAccessibleName(tr("Upcoming singers"));
    setAttribute(Qt::WA_QuitOnClose, false);
    m_videoDisplay = new VideoDisplay(this);
    m_videoDisplay->setObjectName("rotationVideoDisplay");
    m_videoDisplay->setAccessibleName(tr("Karaoke lyrics and video"));
    m_videoDisplay->setAttribute(Qt::WA_TransparentForMouseEvents);
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
    m_names = m_rotation.upcomingSingerNames(m_settings.rotationDisplaySingerCount(),
                                            m_settings.rotationDisplayShowEmptySingers());
    for (auto &name : m_names)
        name = displayName(name);
    const auto upcoming = m_names.isEmpty() ? tr("Waiting for singers") : tr("Up next: %1").arg(m_names.join(", "));
    const auto current = currentSingerName();
    setAccessibleDescription(current.isEmpty() ? upcoming
        : tr("Now singing: %1. %2").arg(current, upcoming));
    update();
}

void RotationDisplay::applySettings()
{
    setFont(m_settings.rotationDisplayFont());
    m_textColor = m_settings.rotationDisplayTextColor();
    m_backgroundColor = m_settings.rotationDisplayBackgroundColor();
    m_showHeading = m_settings.rotationDisplayShowHeading();
    m_hideNameNotes = m_settings.rotationDisplayHideNameNotes();
    m_videoDisplay->setVisible(m_settings.rotationDisplayVideoEnabled());
    updateContentGeometry();
    refresh();
}

void RotationDisplay::updateContentGeometry()
{
    const qreal margin = qBound(12.0, width() * 0.055, 64.0);
    m_singerArea = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
    if (!m_videoDisplay || m_videoDisplay->isHidden())
        return;
    const qreal gap = margin * 0.75;
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
    }
}

void RotationDisplay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), m_backgroundColor);
    const qreal margin = qBound(12.0, width() * 0.055, 64.0);
    QRectF content = m_singerArea;
    QFont displayFont = font();
    const int maxPixels = displayFont.pixelSize() > 0 ? displayFont.pixelSize()
        : qMax(8, qRound(displayFont.pointSizeF() * logicalDpiY() / 72.0));
    const QColor accent("#57d9b2");
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
    painter.setPen(m_currentSinger.isEmpty() ? m_textColor : accent);
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
        QColor fill = m_textColor;
        fill.setAlpha(row == 0 ? 24 : 10);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(rowRect, qMin(12.0, rowHeight * 0.15), qMin(12.0, rowHeight * 0.15));
        QFont rowFont = displayFont;
        rowFont.setBold(row == 0);
        painter.setFont(rowFont);
        const qreal numberWidth = QFontMetricsF(rowFont).horizontalAdvance(QString::number(m_names.size())) + inset;
        painter.setPen(row == 0 ? accent : m_textColor);
        painter.drawText(rowRect.adjusted(inset, 0, -inset, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(row + 1));
        painter.setPen(m_textColor);
        const auto nameRect = rowRect.adjusted(inset + numberWidth, 0, -inset, 0);
        const auto name = QFontMetrics(rowFont).elidedText(m_names[row], Qt::ElideRight,
                                                         qMax(0, qFloor(nameRect.width())));
        painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, name);
    }
}
